#include "mex.hpp"
#include "mexAdapter.hpp"
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
#include <iterator>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

// This define must come before windows.h, otherwise it will include winsock.h, 
// and that will lead to errors when winsock2 is included. 
#define _WINSOCKAPI_
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace matlab::data;
using matlab::mex::ArgumentList;

// Tell the linker to link against the Winsock library
#pragma comment(lib, "Ws2_32.lib")

// Source - https://stackoverflow.com/a/236803
// Posted by Evan Teran, modified by community. See post 'Timeline' for change history
// Retrieved 2026-10-06, License - CC BY-SA 4.0

template <typename Out>
void split(const std::string& s, char delim, Out result) {
    std::istringstream iss(s);
    std::string item;
    while (std::getline(iss, item, delim)) {
        *result++ = item;
    }
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, std::back_inserter(elems));
    return elems;
}

// Force packed layout for eye_data - this is how the data is streamed to us.
#pragma pack(push, 1)
struct eye_data
{
    //    EYE DATA(total 14 bytes)
    char status; //(1 byte)  0 = normal, 1 = calibration
    char calibration_point_number; //(1 bytes)
    unsigned short frame_number; //(2 bytes)
    unsigned short pupil_diameter; //(2 bytes)
    float gaze_x; //(4 bytes)
    float gaze_y; //(4 bytes)
};
typedef struct eye_data eye_data_t;
#pragma pack(pop)

// Validate at compile time that the struct is the expected size
static_assert(sizeof(eye_data_t) == 14, "eye_data_t must be 14 bytes (packed)");

class MexFunction : public matlab::mex::Function 
{
private:
    WSADATA m_wsaData;
    SOCKET m_connectSocket;

    std::shared_ptr<matlab::engine::MATLABEngine> m_matlabPtr;
    ArrayFactory m_factory;
    bool m_connected;
    int m_attempts;

    // Reader thread - polls readEyeDataFromSocket() at READER_RATE_HZ, saves latest sample.
    static constexpr int READER_RATE_HZ = 120;
    std::thread m_readerThread;
    std::atomic<bool> m_stopReader;
    std::atomic<bool> m_readerFailed;   // set by the reader thread when it exits due to an error
    // Pending "reader stopped" message queued by the reader thread. Only touched by the main
    // thread after joining the reader thread.
    matlab::engine::FutureResult<std::vector<Array>> m_notifyFuture;
    std::mutex m_eyeDataMutex;      // protects m_latestEyeData, m_haveEyeData, m_readerError
    eye_data_t m_latestEyeData;
    bool m_haveEyeData;
    std::string m_readerError;      // non-empty if the reader thread exited due to an error

public:
    MexFunction(): m_connectSocket(INVALID_SOCKET), m_connected(false), m_matlabPtr(getEngine()), m_attempts(0),
        m_stopReader(false), m_readerFailed(false), m_latestEyeData(), m_haveEyeData(false)
    {
        // 1. Initialize Winsock
        int iResult = WSAStartup(MAKEWORD(2, 2), &m_wsaData);
        if (iResult != 0) 
        {
            std::stringstream out;
            out << "WSAStartup failed with error: " << iResult;
            m_matlabPtr->feval(u"error", 0,
                std::vector<Array>({ m_factory.createScalar(out.str())}));
        }

        // 2. Create a Socket
        m_connectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_connectSocket == INVALID_SOCKET) {
            WSACleanup();
            std::stringstream out;
            out << "Winsock socket creation failed with error: " << WSAGetLastError();
            m_matlabPtr->feval(u"error", 0,
                std::vector<Array>({ m_factory.createScalar(out.str()) }));

        }
    };

    ~MexFunction()
    {
        stopReaderThread();

        // If the "reader stopped" message hasn't run yet, cancel it. Otherwise MATLAB
        // completes it after this MEX file is unloaded and crashes.
        if (m_notifyFuture.valid() &&
            m_notifyFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            m_notifyFuture.cancel();
        }
        if (m_connectSocket != INVALID_SOCKET)
        {
            closesocket(m_connectSocket);
            m_connectSocket = INVALID_SOCKET;
        }
    };

    void operator()(ArgumentList outputs, ArgumentList inputs)
    {

        // Validate arguments
        if (inputs[0].getType() != ArrayType::CHAR) 
        {
            m_matlabPtr->feval(u"error", 0,
                std::vector<Array>({ m_factory.createScalar("Input command must be char") }));
        }
        matlab::data::CharArray cmd = inputs[0];
        std::string s = cmd.toAscii();
        // std::cout << "cmd " << s << std::endl;

        // If the reader thread died since the last call, finish disconnecting now.
        if (m_readerFailed)
        {
            disconnectFromASL();
        }

        if (s == "connect") 
        {
            m_attempts++;
            
            // connected already?
            if (m_connected)
            {
                merror(std::string("Cannot 'connect' - already connected"));
            }

            // Expecting a single arg string (address:port)
            if (inputs.size() != 2 || inputs[1].getType() != ArrayType::CHAR)
            {
				merror(std::string("'connect' requires one additional arg: 'addr:port'"));
            }

			// Split address and port, and check that port is an integer
            std::string sAddr = ((matlab::data::CharArray)inputs[1]).toAscii();
            std::vector<std::string> vAddr = split(sAddr, ':');
            size_t port = 0;
            if (vAddr.size() != 2)
            {
                merror(std::string("'connect' address arg must be in form: 'addr:port'"));
            }

            std::cout << "Connecting to " << vAddr[0] << ":" << vAddr[1] << std::endl;
            m_connected = connectToASL(vAddr[0], vAddr[1]);
            if (m_connected)
            {
                startReaderThread();
            }
        }
        else if (s == "status")
        {
            std::cout << "Connected: " << m_connected << std::endl;
            std::string readerError;
            {
                std::lock_guard<std::mutex> lock(m_eyeDataMutex);
                readerError = m_readerError;
            }
            if (!readerError.empty())
            {
                std::cout << "Reader thread stopped: " << readerError << std::endl;
            }
        }
		else if (s == "read")
		{
			eye_data_t ed = getLatestEyeData(s);

			// Print the contents of the struct
			std::stringstream out;
			out << "eye_data_t: ";
			out << "status=" << static_cast<int>(ed.status) << "; ";
			out << "calibration_point_number=" << static_cast<int>(ed.calibration_point_number) << "; ";
			out << "frame_number=" << ed.frame_number << "; ";
			out << "pupil_diameter=" << ed.pupil_diameter << "; ";
			out << "gaze_x=" << ed.gaze_x << "; ";
			out << "gaze_y=" << ed.gaze_y;

			// Use MATLAB disp to show the string
			m_matlabPtr->feval(u"disp", 0, std::vector<Array>({ m_factory.createScalar(out.str()) }));
		}
		else if (s == "eyepos")
		{
			eye_data_t ed = getLatestEyeData(s);

			// Return the sample as a 1x1 struct. Values are doubles so they're easy to use in MATLAB.
			StructArray result = m_factory.createStructArray({ 1, 1 },
				{ "status", "calibration_point_number", "frame_number", "pupil_diameter", "gaze_x", "gaze_y" });
			result[0]["status"] = m_factory.createScalar(static_cast<double>(ed.status));
			result[0]["calibration_point_number"] = m_factory.createScalar(static_cast<double>(ed.calibration_point_number));
			result[0]["frame_number"] = m_factory.createScalar(static_cast<double>(ed.frame_number));
			result[0]["pupil_diameter"] = m_factory.createScalar(static_cast<double>(ed.pupil_diameter));
			result[0]["gaze_x"] = m_factory.createScalar(static_cast<double>(ed.gaze_x));
			result[0]["gaze_y"] = m_factory.createScalar(static_cast<double>(ed.gaze_y));
			outputs[0] = std::move(result);
		}
		else if (s == "xdat")
		{
			if (!m_connected)
			{
				merror(std::string("Cannot 'xdat' - not connected"));
			}
			if (inputs.size() > 2)
			{
				merror(std::string("'xdat' takes at most one additional arg: a numeric scalar"));
			}

			uint16_t value = 0;
			if (inputs.size() == 2)
			{
				ArrayType t = inputs[1].getType();
				bool isNumeric = t == ArrayType::DOUBLE || t == ArrayType::SINGLE || t == ArrayType::LOGICAL ||
					t == ArrayType::INT8 || t == ArrayType::UINT8 || t == ArrayType::INT16 || t == ArrayType::UINT16 ||
					t == ArrayType::INT32 || t == ArrayType::UINT32 || t == ArrayType::INT64 || t == ArrayType::UINT64;
				if (!isNumeric || inputs[1].getNumberOfElements() != 1)
				{
					merror(std::string("'xdat' arg must be a numeric scalar"));
				}
				// Use MATLAB's uint16() so rounding and saturation match MATLAB exactly.
				TypedArray<uint16_t> v = m_matlabPtr->feval(u"uint16", inputs[1]);
				value = v[0];
			}

			std::string errmsg;
			if (!sendToSocket(reinterpret_cast<const char*>(&value), sizeof(value), errmsg))
			{
				merror(errmsg);
			}
		}
		else if (s == "disconnect")
		{
			if (!m_connected)
			{
				merror(std::string("Cannot 'disconnect' - not connected"));
			}
			disconnectFromASL();
		}
		else if (s == "cleanup")
		{
			if (m_connected)
			{
				disconnectFromASL();
			}
			WSACleanup();
		}
		else
		{
			std::stringstream out;
			out << "Unknown command: " << s;
		    merror(std::string("Unknown command: ") + s);
		}

    }
    bool connectToASL(const std::string& addr, const std::string& portStr)
    {
        // Close any existing socket created earlier
        if (m_connectSocket != INVALID_SOCKET) {
            closesocket(m_connectSocket);
            m_connectSocket = INVALID_SOCKET;
        }

        struct addrinfo hints;
        struct addrinfo *result = nullptr, *ptr = nullptr;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;      // Allow IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM;  // TCP
        hints.ai_protocol = IPPROTO_TCP;

        int rv = getaddrinfo(addr.c_str(), portStr.c_str(), &hints, &result);
        if (rv != 0) {
            std::stringstream out;
            out << "getaddrinfo failed: " << gai_strerrorA(rv);
            merror(out.str());
            return false;
        }

        // Save the error as soon as a call fails - closesocket() resets WSAGetLastError().
        int lastError = 0;
        for (ptr = result; ptr != nullptr; ptr = ptr->ai_next)
        {
            m_connectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (m_connectSocket == INVALID_SOCKET) {
                lastError = WSAGetLastError();
                continue;
            }

            // Attempt to connect
            int iResult = connect(m_connectSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
            if (iResult == SOCKET_ERROR) {
                lastError = WSAGetLastError();
                closesocket(m_connectSocket);
                m_connectSocket = INVALID_SOCKET;
                // try next address
                continue;
            }

            // Successfully connected
            break;
        }

        freeaddrinfo(result);

        if (m_connectSocket == INVALID_SOCKET) {
            std::stringstream out;
            out << "Unable to connect to server. Last error: " << lastError;
            merror(out.str());
            return false;
        }

        std::cout << "Connected to host." << std::endl;
        return true;
    };

    // Returns a copy of the latest sample saved by the reader thread. Raises a MATLAB
    // error (naming the command cmd) if not connected or no sample is available.
    eye_data_t getLatestEyeData(const std::string& cmd)
    {
        if (!m_connected)
        {
            merror(std::string("Cannot '") + cmd + "' - not connected");
        }

        eye_data_t ed;
        bool haveData;
        std::string readerError;
        {
            std::lock_guard<std::mutex> lock(m_eyeDataMutex);
            ed = m_latestEyeData;
            haveData = m_haveEyeData;
            readerError = m_readerError;
        }

        if (!readerError.empty())
        {
            merror(std::string("Reader thread stopped: ") + readerError);
        }
        if (!haveData)
        {
            merror(std::string("No eye data received yet"));
        }
        return ed;
    };

    // Writes len bytes to the socket. Safe to call while the reader thread is in recv().
    bool sendToSocket(const char* buf, size_t len, std::string& errmsg)
    {
        if (m_connectSocket == INVALID_SOCKET) {
            errmsg = "Socket is not connected.";
            return false;
        }

        size_t sent = 0;
        while (sent < len) {
            int r = send(m_connectSocket, buf + sent, static_cast<int>(len - sent), 0);
            if (r == SOCKET_ERROR) {
                std::stringstream out;
                out << "send failed with error: " << WSAGetLastError();
                errmsg = out.str();
                return false;
            }
            sent += static_cast<size_t>(r);
        }
        return true;
    };

    // Waits until the socket has data to read (or the peer has closed it). Waits in
    // short steps so the reader thread notices m_stopReader promptly - on Windows,
    // shutdown() from another thread doesn't reliably wake a blocked recv(), so
    // without this, stopping the thread could hang until the tracker sent more data.
    // Returns false and fills in errmsg if stopped or select() fails.
    bool waitForSocketData(std::string& errmsg)
    {
        while (!m_stopReader) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(m_connectSocket, &readSet);
            timeval timeout = { 0, 50000 };     // 50 ms
            int r = select(0, &readSet, nullptr, nullptr, &timeout);
            if (r == SOCKET_ERROR) {
                std::stringstream out;
                out << "select failed with error: " << WSAGetLastError();
                errmsg = out.str();
                return false;
            }
            if (r > 0) {
                return true;
            }
        }
        errmsg = "Reader stopped.";
        return false;
    };

    // Reads one eye_data_t from the socket. Called from the reader thread, so it must
    // not call into MATLAB - on failure it returns false and fills in errmsg.
    bool readEyeDataFromSocket(eye_data_t& ed, std::string& errmsg)
    {
        if (m_connectSocket == INVALID_SOCKET) {
            errmsg = "Socket is not connected.";
            return false;
        }

        size_t expected = sizeof(ed);
        char* buf = reinterpret_cast<char*>(&ed);
        size_t received = 0;

        while (received < expected) {
            if (!waitForSocketData(errmsg)) {
                return false;
            }
            int r = recv(m_connectSocket, buf + received, static_cast<int>(expected - received), 0);
            if (r == 0) {
                errmsg = "Connection closed by peer while reading eye_data_t.";
                return false;
            }
            if (r == SOCKET_ERROR) {
                std::stringstream out;
                out << "recv failed with error: " << WSAGetLastError();
                errmsg = out.str();
                return false;
            }
            received += static_cast<size_t>(r);
        }
        return true;
    };

    void startReaderThread()
    {
        stopReaderThread();
        {
            std::lock_guard<std::mutex> lock(m_eyeDataMutex);
            m_haveEyeData = false;
            m_readerError.clear();
        }
        m_stopReader = false;
        m_readerFailed = false;
        m_readerThread = std::thread(&MexFunction::readerLoop, this);
    };

    void disconnectFromASL()
    {
        stopReaderThread();
        if (m_connectSocket != INVALID_SOCKET) {
            closesocket(m_connectSocket);
            m_connectSocket = INVALID_SOCKET;
        }
        m_connected = false;
        m_readerFailed = false;
    };

    void stopReaderThread()
    {
        if (!m_readerThread.joinable()) return;

        // The reader thread checks this flag at least every 50 ms (see waitForSocketData).
        m_stopReader = true;
        m_readerThread.join();
    };

    void readerLoop()
    {
        const auto period = std::chrono::microseconds(1000000 / READER_RATE_HZ);
        auto next = std::chrono::steady_clock::now();

        while (!m_stopReader)
        {
            // Block for one sample, then drain any complete samples already buffered
            // so that the saved value is the most recent one available.
            eye_data_t ed;
            std::string errmsg;
            bool ok = readEyeDataFromSocket(ed, errmsg);
            while (ok)
            {
                u_long available = 0;
                if (ioctlsocket(m_connectSocket, FIONREAD, &available) == SOCKET_ERROR) {
                    std::stringstream out;
                    out << "ioctlsocket(FIONREAD) failed: " << WSAGetLastError();
                    errmsg = out.str();
                    ok = false;
                }
                else if (available < sizeof(eye_data_t)) {
                    break;      // only a partial sample (or nothing) left - pick it up next pass
                }
                else {
                    ok = readEyeDataFromSocket(ed, errmsg);
                }
            }

            if (!ok)
            {
                // Errors caused by our own shutdown() are expected - don't report them.
                if (!m_stopReader)
                {
                    {
                        std::lock_guard<std::mutex> lock(m_eyeDataMutex);
                        m_readerError = errmsg;
                    }
                    m_readerFailed = true;

                    // MATLAB can't be called synchronously from this thread. fevalAsync queues
                    // the call to run on MATLAB's main thread. Don't wait on the result - the
                    // main thread may be blocked in this MEX function.
                    ArrayFactory factory;
                    m_notifyFuture = m_matlabPtr->fevalAsync(u"fprintf", 0, std::vector<Array>({
                        factory.createScalar("%s\n"),
                        factory.createScalar("asl: reader stopped (" + errmsg + "). Disconnected.") }));
                }
                return;
            }

            {
                std::lock_guard<std::mutex> lock(m_eyeDataMutex);
                m_latestEyeData = ed;
                m_haveEyeData = true;
            }

            next += period;
            auto now = std::chrono::steady_clock::now();
            if (next < now) {
                next = now;     // fell behind (e.g. recv blocked) - don't try to catch up
            }
            std::this_thread::sleep_until(next);
        }
    };

    void flushInputBuffer()
    {
        if (m_connectSocket == INVALID_SOCKET) {
            merror(std::string("Socket is not connected."));
            return;
        }

        u_long available = 0;
        int rc = ioctlsocket(m_connectSocket, FIONREAD, &available);
        if (rc == SOCKET_ERROR) {
            std::stringstream out;
            out << "ioctlsocket(FIONREAD) failed: " << WSAGetLastError();
            merror(out.str());
            return;
        }

        size_t totalFlushed = 0;
        while (available > 0) {
            int toRead = available > 4096 ? 4096 : static_cast<int>(available);
            std::vector<char> tmp(static_cast<size_t>(toRead));
            int r = recv(m_connectSocket, tmp.data(), toRead, 0);
            if (r == 0) {
                merror(std::string("Connection closed by peer while flushing input buffer."));
                return;
            }
            if (r == SOCKET_ERROR) {
                int lastErr = WSAGetLastError();
                if (lastErr == WSAEWOULDBLOCK) break;
                std::stringstream out;
                out << "recv failed while flushing: " << lastErr;
                merror(out.str());
                return;
            }
            totalFlushed += static_cast<size_t>(r);

            rc = ioctlsocket(m_connectSocket, FIONREAD, &available);
            if (rc == SOCKET_ERROR) break;
        }

        std::stringstream out;
        out << "Flushed " << totalFlushed << " bytes from socket input buffer.";
        m_matlabPtr->feval(u"disp", 0, std::vector<Array>({ m_factory.createScalar(out.str()) }));
    };

    void merror(char *msg)
    {
        merror(std::string(msg));
        return;
    };

    void merror(std::string& errmsg)
    {
        m_matlabPtr->feval(u"error", 0,
            std::vector<Array>({ m_factory.createScalar(errmsg) }));
    };
};