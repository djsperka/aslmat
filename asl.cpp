#include "mex.hpp"
#include "mexAdapter.hpp"

// compiler error "Please define _WIN32_WINNT or _WIN32_WINDOWS appropriately."
// Source - https://stackoverflow.com/a/43832497
// Posted by Hill
// Retrieved 2026-09-10, License - CC BY-SA 3.0
#include <SDKDDKVer.h>

#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <string>
#include <iostream>
#include <sstream>

using namespace matlab::data;
using matlab::mex::ArgumentList;
using boost::asio::ip::tcp;




class MexFunction : public matlab::mex::Function 
{
private:
    boost::asio::io_context m_io_context;
    boost::asio::ip::tcp::socket m_socket;
    std::shared_ptr<matlab::engine::MATLABEngine> m_matlabPtr;
    ArrayFactory m_factory;
    bool m_connected;
    int m_attempts;

public:
    MexFunction(): m_socket(m_io_context), m_connected(false), m_matlabPtr(getEngine()), m_attempts(0) {};
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
                m_matlabPtr->feval(u"error", 0,
                    std::vector<Array>({ m_factory.createScalar("Cannot 'connect' - already connected") }));
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
            // vAddr[1] is the port
            m_connected = connect(vAddr[0], vAddr[1]);
        }
        else if (s == "status")
        {
            std::cout << "Connected: " << m_connected << std::endl;
            std::cout << "Attempts: " << m_attempts << std::endl;
        }

        // Assign outputs
        // outputs[0] = 10;
    }

    bool connect(const std::string& addr, const std::string& port)
    {
        // Error to not throw exception
        boost::system::error_code not_throw;

        // Resolve hostname and port
        boost::asio::ip::tcp::resolver resolver(m_io_context);
        boost::asio::ip::tcp::resolver::query query(addr, port);
        boost::asio::ip::tcp::resolver::iterator endpoint = resolver.resolve(query, not_throw);
        if (not_throw) 
        {
            std::stringstream out;
            out << "Error resolving host(" << not_throw.value() << "): "<< not_throw.message();
            m_matlabPtr->feval(u"error", 0,
                    std::vector<Array>({ m_factory.createScalar(out.str()) }));
        }

        // Socket and connection
        boost::asio::connect(m_socket, endpoint, not_throw);
        if (not_throw) 
        {
            std::stringstream out;
            out << "Error connecting(" << not_throw.value() << "): "<< not_throw.message();
            m_matlabPtr->feval(u"error", 0,
                    std::vector<Array>({ m_factory.createScalar(out.str()) }));
        }
        std::cout << "Connected to host." << std::endl;
    }

};
