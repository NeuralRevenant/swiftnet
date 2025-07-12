#include "http/http_server.hpp"
#include "io_awaitable.hpp"
#include "io_context.hpp"
#include <array>
#include <sstream>
#include <string_view>
#include <iostream>

using namespace swiftnet;
using namespace swiftnet::http;

static bool parse_request(const std::string &data, request &req, std::size_t &consumed)
{
    auto pos_end = data.find("\r\n\r\n");
    if (pos_end == std::string::npos)
        return false; // incomplete
    std::string_view header_view(data.data(), pos_end + 4);
    consumed = pos_end + 4;

    std::string header_str(header_view);
    std::istringstream iss(header_str);
    std::string line;
    if (!std::getline(iss, line))
        return false;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    std::istringstream l0(line);
    if (!(l0 >> req.method >> req.path))
        return false;
    // header lines
    while (std::getline(iss, line))
    {
        if (line == "\r" || line == "" || line == "\n")
            break;
        if (line.back() == '\r')
            line.pop_back();
        auto colon = line.find(":");
        if (colon == std::string::npos)
            continue;
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ')
            value.erase(0, 1);
        req.headers[std::move(key)] = std::move(value);
    }
    // simple: no body parsing (only GET/HEAD)
    req.body.clear();
    return true;
}

std::string response::to_string() const
{
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " OK\r\n";
    if (headers.find("Content-Length") == headers.end())
        oss << "Content-Length: " << body.size() << "\r\n";
    for (const auto &[k, v] : headers)
        oss << k << ": " << v << "\r\n";
    oss << "\r\n";
    oss << body;
    return oss.str();
}

server::server(uint16_t port, int backlog) : acceptor_(port, backlog) 
{
    std::cout << "[DEBUG] HTTP server constructor called with port=" << port << " backlog=" << backlog << std::endl;
}

server::~server() { 
    std::cout << "[DEBUG] HTTP server destructor called" << std::endl;
    stop(); 
}

void server::route(const std::string &method, const std::string &path, handler_t h)
{
    routes_.insert_or_assign(route_key{method, path}, std::move(h));
}

void server::start(std::size_t threads)
{
    std::cout << "[DEBUG] HTTP server::start() called with " << threads << " threads" << std::endl;
    
    if (running_)
        return;
    running_ = true;
    
    std::cout << "[DEBUG] Starting I/O context..." << std::endl;
    io_context::instance().start();
    
    std::cout << "[DEBUG] Starting virtual thread scheduler..." << std::endl;
    vthread_scheduler::instance().start(threads);
    
    auto handler = [this](net::tcp_socket sock) {
        vthread_scheduler::instance().schedule(client_task(std::move(sock)));
    };
    
    std::cout << "[DEBUG] Scheduling acceptor async_accept..." << std::endl;
    
    // CRITICAL FIX: Ensure only one acceptor supervisor runs
    bool expected = false;
    if (!acceptor_supervisor_running_.compare_exchange_strong(expected, true)) {
        std::cout << "[DEBUG] Acceptor supervisor already running, skipping..." << std::endl;
        return;
    }
    
    // Schedule a task that restarts the acceptor if it completes
    vthread_scheduler::instance().schedule([this, handler]() -> vthread {
        std::cout << "[DEBUG] Acceptor supervisor started" << std::endl;
        
        while (running_) {
            try {
                std::cout << "[DEBUG] Starting acceptor coroutine..." << std::endl;
                co_await acceptor_.async_accept(handler);
                std::cout << "[DEBUG] Acceptor coroutine completed normally, restarting..." << std::endl;
            } catch (const std::exception& e) {
                std::cout << "[DEBUG] Acceptor exception: " << e.what() << ", restarting after delay..." << std::endl;
                // Small delay before restarting to avoid tight loop on persistent errors
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        
        std::cout << "[DEBUG] Acceptor supervisor exiting (server stopped)" << std::endl;
        acceptor_supervisor_running_ = false;
        co_return;
    }());
    
    std::cout << "[DEBUG] HTTP server started successfully" << std::endl;
}

void server::stop()
{
    running_ = false;
}

vthread server::client_task(net::tcp_socket sock)
{
    std::array<char, 8192> buf;
    std::string accum;
    bool keep_alive = true;
    while (keep_alive)
    {
        // ensure at least one full request in buffer
        if (accum.find("\r\n\r\n") == std::string::npos)
        {
            int n = co_await sock.async_read(buf.data(), buf.size());
            if (n <= 0)
            {
                sock.close();
                co_return;
            }
            accum.append(buf.data(), n);
            continue;
        }

        while (true)
        {
            request req;
            std::size_t consumed = 0;
            if (!parse_request(accum, req, consumed))
                break; // incomplete

            // remove parsed request from buffer
            accum.erase(0, consumed);

            // Determine keep-alive state
            auto conn_hdr = req.headers.find("Connection");
            bool client_keep = conn_hdr != req.headers.end() && (conn_hdr->second == "keep-alive" || conn_hdr->second == "Keep-Alive");

            response res;
            auto it = routes_.find(route_key{req.method, req.path});
            if (it != routes_.end())
            {
                it->second(req, res);
            }
            else
            {
                // Try catch-all route
                auto catch_all_it = routes_.find(route_key{"*", "*"});
                if (catch_all_it != routes_.end())
                {
                    catch_all_it->second(req, res);
                }
                else
                {
                res.status = 404;
                res.body = "Not Found";
                res.headers["Content-Type"] = "text/plain";
                }
            }

            if (client_keep)
                res.headers["Connection"] = "keep-alive";
            else
                res.headers["Connection"] = "close";

            std::string out = res.to_string();
            int w = co_await sock.async_write(out.data(), out.size());
            (void)w;

            if (!client_keep)
            {
                keep_alive = false;
                break; // will close
            }

            // loop to parse next request in buffer if any; otherwise read more
            if (accum.find("\r\n\r\n") == std::string::npos)
                break;
        }
    }
    sock.close();
    co_return;
}
