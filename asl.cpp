#include "mex.hpp"
#include "mexAdapter.hpp"
#include <string>
#include <iostream>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

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
//                m_matlabPtr->feval(u"error", 0,
//                    std::vector<Array>({ m_factory.createScalar("Cannot 'connect' - already connected") }));
            }

            // Expecting a string (address:port)
            if (inputs.size() != 2 || inputs[1].getType() != ArrayType::CHAR)
            {
                m_matlabPtr->feval(u"error", 0,
                    std::vector<Array>({ m_factory.createScalar("'connect' requires one additional arg: 'addr:port'") }));
            }

            // Source - https://stackoverflow.com/a/5734491
            // Posted by karlphillip, modified by community. See post 'Timeline' for change history
            // Retrieved 2026-09-10, License - CC BY-SA 4.0
            
            std::string sAddr = ((matlab::data::CharArray)inputs[1]).toAscii();
            std::vector<std::string> vAddr;
            boost::split(vAddr, sAddr, boost::is_any_of(":"));

            if (vAddr.size() != 2)
            {
                m_matlabPtr->feval(u"error", 0,
                    std::vector<Array>({ m_factory.createScalar("'connect' address arg must be in form: 'addr:port'") }));
            }

            // vAddr[0] is the address
            // vAddr[1] is the port - check that it is an int
            int port = 0;
            try
            {
                port = boost::lexical_cast<int>(vAddr[1]);
            }
            catch(boost::bad_lexical_cast &)
            {
                m_matlabPtr->feval(u"error", 0,
                    std::vector<Array>({ m_factory.createScalar("'connect' port must be an integer.") }));
            }
            m_connected = connectToASL(vAddr[0], port);
        }
        else if (s == "status")
        {
            std::cout << "Connected: " << m_connected << std::endl;
            std::cout << "Attempts: " << m_attempts << std::endl;
        }

    }

    bool connectToASL(const std::string& addr, int port)
    {
        sockaddr_in clientService;
        clientService.sin_family = AF_INET;
        //clientService.sin_addr.s_addr = inet_addr("127.0.0.1");
        InetPton(AF_INET, addr.c_str(), &clientService.sin_addr.s_addr);
        clientService.sin_port = htons(8080);
    
        // 4. Connect to Server
        // Note: For production, set timeouts or use non-blocking mode to keep MATLAB responsive
        u_long block = 1;
        ioctlsocket(m_connectSocket, FIONBIO, &block);
        int iResult = connect(m_connectSocket, (SOCKADDR*)&clientService, sizeof(clientService));
        if (iResult == SOCKET_ERROR) 
        {
            if (WSAGetLastError() != WSAEWOULDBLOCK) 
            {
                closesocket(m_connectSocket);
                WSACleanup();
                merror("Connecting to server failed.");
            }
            else
            {
                // 3. Use select() to implement the timeout
                fd_set setW, setE;
                FD_ZERO(&setW); FD_SET(m_connectSocket, &setW);
                FD_ZERO(&setE); FD_SET(m_connectSocket, &setE);

                timeval time_out;
                time_out.tv_sec = 5;  // 5-second timeout
                time_out.tv_usec = 0;

                int ret = select(0, NULL, &setW, &setE, &time_out);
                if (ret <= 0) 
                {
                    // ret == 0 means timeout elapsed; ret < 0 means select failed
                    closesocket(m_connectSocket);
                    if (ret == 0) WSASetLastError(WSAETIMEDOUT);
                    merror("Timed out connecting to ASL.");
                }

                // 4. Check if an error occurred on the socket
                if (FD_ISSET(m_connectSocket, &setE)) 
                {
                    int err = 0;
                    int len = sizeof(err);
                    getsockopt(m_connectSocket, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                    closesocket(m_connectSocket);
                    WSASetLastError(err);
                    std::stringstream out;
                    out << "Error on socket: " << err;
                    merror(out.str());
                }
            }
        }
        // 5. Connection succeeded! Return the socket to blocking mode if desired
        block = 0;
        ioctlsocket(m_connectSocket, FIONBIO, &block);
        std::cout << "Connected to host." << std::endl;
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