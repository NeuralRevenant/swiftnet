#ifndef http_server_hpp
#define http_server_hpp

#include "../net/acceptor.hpp"
#include "../net/tcp_socket.hpp"
#include "../vthread_scheduler.hpp"
#include "../vthread.hpp"
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <atomic>

namespace swiftnet::http
{

    struct request
    {
        std::string method;
        std::string path;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    struct response
    {
        int status{200};
        std::map<std::string, std::string> headers;
        std::string body;

        std::string to_string() const;
    };

    class server
    {
    public:
        using handler_t = std::function<void(const request &, response &)>;

        explicit server(uint16_t port = 8080, int backlog = 1024);
        ~server();

        void route(const std::string &method, const std::string &path, handler_t h);

        void start(std::size_t threads = std::thread::hardware_concurrency());
        void stop();

    private:
        struct route_key
        {
            std::string method;
            std::string path;
            bool operator<(const route_key &o) const noexcept
            {
                return method < o.method || (method == o.method && path < o.path);
            }
        };

        vthread client_task(net::tcp_socket sock);

        net::acceptor acceptor_;
        std::map<route_key, handler_t> routes_;
        std::atomic<bool> running_{false};
        std::atomic<bool> acceptor_supervisor_running_{false}; // Prevent multiple supervisors
    };

} // namespace swiftnet::http

#endif
