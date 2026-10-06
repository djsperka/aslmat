#include "mex.hpp"
#include "mexAdapter.hpp"
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
#include <iterator>

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


class MexFunction : public matlab::mex::Function 
{
private:
    WSADATA m_wsaData;
    SOCKET m_connectSocket;

    std::shared_ptr<matlab::engine::MATLABEngine> m_matlabPtr;
    ArrayFactory m_factory;
    bool m_connected;
    int m_attempts;

public:
    MexFunction(): m_connectSocket(INVALID_SOCKET), m_connected(false), m_matlabPtr(getEngine()), m_attempts(0) 
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
        std::cout << "cmd " << s << std::endl;

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
        }
        else if (s == "status")
        {
            std::cout << "Connected: " << m_connected << std::endl;
            std::cout << "Attempts: " << m_attempts << std::endl;
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

        for (ptr = result; ptr != nullptr; ptr = ptr->ai_next)
        {
            m_connectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (m_connectSocket == INVALID_SOCKET) {
                continue;
            }

            // Attempt to connect
            int iResult = connect(m_connectSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
            if (iResult == SOCKET_ERROR) {
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
            out << "Unable to connect to server. Last error: " << WSAGetLastError();
            merror(out.str());
            return false;
        }

        std::cout << "Connected to host." << std::endl;
        return true;
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