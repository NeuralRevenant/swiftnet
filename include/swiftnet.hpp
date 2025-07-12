#ifndef SWIFTNET_HPP
#define SWIFTNET_HPP

#include "http/http_server.hpp"
#include "net/tcp_socket.hpp"
#include "vthread.hpp"
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <condition_variable>
#include <mutex>

namespace swiftnet
{

    // Forward declarations
    class SwiftNet;
    class Request;
    class Response;

    // Type aliases
    using Json = nlohmann::json;
    using middleware_t = std::function<void(Request &, Response &, std::function<void()>)>;
    using handler_t = std::function<void(Request &, Response &)>;

    // Request class
    class Request
    {
    public:
        explicit Request(const http::request &req);

        const std::string &method() const { return method_; }
        const std::string &path() const { return path_; }
        const std::string &body() const { return body_; }
        const std::unordered_map<std::string, std::string> &headers() const { return headers_; }

        // Get header value
        std::string header(const std::string &name) const;

        // Get query parameter
        std::string query(const std::string &name) const;

        // Get route parameter (set by router)
        std::string param(const std::string &name) const;
        void set_param(const std::string &name, const std::string &value);

        // JSON parsing
        bool is_json() const;
        Json json() const;

        // Form data parsing
        std::unordered_map<std::string, std::string> form() const;

        // File operations
        bool has_file(const std::string &field) const;

    private:
        std::string method_;
        std::string path_;
        std::string body_;
        std::unordered_map<std::string, std::string> headers_;
        std::unordered_map<std::string, std::string> query_params_;
        std::unordered_map<std::string, std::string> route_params_;
        mutable Json json_cache_;
        mutable bool json_parsed_{false};

        void parse_query_string();
    };

    // Response class
    class Response
    {
    public:
        Response();

        // Status
        Response &status(int code);
        int status() const { return status_; }

        // Headers
        Response &header(const std::string &name, const std::string &value);
        Response &headers(const std::unordered_map<std::string, std::string> &headers);

        // Content
        Response &text(const std::string &content);
        Response &html(const std::string &content);
        Response &json(const Json &data);
        Response &file(const std::string &filepath);
        Response &send(const std::string &content);

        // Convenience methods
        Response &ok(const std::string &content = "");
        Response &created(const Json &data = Json{});
        Response &bad_request(const std::string &message = "Bad Request");
        Response &unauthorized(const std::string &message = "Unauthorized");
        Response &forbidden(const std::string &message = "Forbidden");
        Response &not_found(const std::string &message = "Not Found");
        Response &internal_error(const std::string &message = "Internal Server Error");

        // Redirect
        Response &redirect(const std::string &url, int code = 302);

        // Cookies
        Response &cookie(const std::string &name, const std::string &value,
                        const std::string &path = "/", int max_age = 0);

        // Convert to HTTP response
        http::response to_http_response() const;

    private:
        int status_;
        std::unordered_map<std::string, std::string> headers_;
        std::string body_;
    };

    // Route structure
    struct Route
    {
        std::string method;
        std::string pattern;
        std::regex regex;
        std::vector<std::string> param_names;
        handler_t handler;
    };

    // SwiftNet class
    class SwiftNet
    {
    public:
        explicit SwiftNet(uint16_t port = 8080);
        ~SwiftNet();

        // HTTP methods
        SwiftNet &get(const std::string &path, handler_t handler);
        SwiftNet &post(const std::string &path, handler_t handler);
        SwiftNet &put(const std::string &path, handler_t handler);
        SwiftNet &del(const std::string &path, handler_t handler);
        SwiftNet &patch(const std::string &path, handler_t handler);
        SwiftNet &options(const std::string &path, handler_t handler);
        SwiftNet &head(const std::string &path, handler_t handler);

        // Middleware
        SwiftNet &use(middleware_t middleware);
        SwiftNet &use(const std::string &path, middleware_t middleware);

        // Static files
        SwiftNet &static_files(const std::string &path, const std::string &root);

        // Convenience middleware
        SwiftNet &cors(const std::string &origin = "*");
        SwiftNet &json(size_t limit = 1024 * 1024); // 1MB default
        SwiftNet &logger();

        // Server control
        void listen(std::function<void()> callback = nullptr);
        void listen(uint16_t port, std::function<void()> callback = nullptr);
        void close();

        // Configuration
        SwiftNet &set_threads(size_t threads);
        SwiftNet &set_backlog(int backlog);

    private:
        uint16_t port_;
        size_t threads_;
        int backlog_;
        bool running_;
        
        // Blocking mechanism for listen()
        std::condition_variable shutdown_cv_;
        std::mutex shutdown_mutex_;
        bool shutdown_requested_{false};

        std::vector<Route> routes_;
        std::vector<middleware_t> middlewares_;
        std::vector<std::pair<std::string, middleware_t>> path_middlewares_;
        std::unique_ptr<http::server> server_;

        void handle_request(const http::request &req, http::response &res);
        bool match_route(const Route &route, const std::string &method,
                        const std::string &path, Request &request);
        Route create_route(const std::string &method, const std::string &pattern, handler_t handler);
        void apply_middlewares(Request &req, Response &res, handler_t final_handler);
    };

    // Utility functions
    namespace utils
    {
        std::string url_decode(const std::string &str);
        std::string url_encode(const std::string &str);
        std::unordered_map<std::string, std::string> parse_query_string(const std::string &query);
        std::string mime_type(const std::string &filepath);
        std::string read_file(const std::string &filepath);
        bool file_exists(const std::string &filepath);
        size_t file_size(const std::string &filepath);
        Json parse_json(const std::string &str);
        std::string serialize_json(const Json &json);
    }

    // WebSocket support (basic structure for future implementation)
    namespace ws
    {
        class WebSocket;
        using ws_handler_t = std::function<void(WebSocket &)>;
        using ws_message_handler_t = std::function<void(WebSocket &, const std::string &)>;
        using ws_close_handler_t = std::function<void(WebSocket &)>;

        class WebSocketServer
        {
        public:
            explicit WebSocketServer(SwiftNet &app);
            void on_connection(ws_handler_t handler);
            void on_message(ws_message_handler_t handler);
            void on_close(ws_close_handler_t handler);

        private:
            SwiftNet &app_;
            ws_handler_t connection_handler_;
            ws_message_handler_t message_handler_;
            ws_close_handler_t close_handler_;
        };
    }

    // Logger singleton
    class Logger
    {
    public:
        static Logger &instance();
        void info(const std::string &message);
        void warn(const std::string &message);
        void error(const std::string &message);
        void debug(const std::string &message);

    private:
        Logger();
        std::shared_ptr<spdlog::logger> logger_;
    };

    // MIME type mappings
    extern const std::unordered_map<std::string, std::string> MIME_TYPES;
}

#endif // SWIFTNET_HPP 