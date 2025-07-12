#include "swiftnet.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <chrono>

using namespace swiftnet;

// MIME type mappings
const std::unordered_map<std::string, std::string> swiftnet::MIME_TYPES = {
    {".html", "text/html"},
    {".htm", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".json", "application/json"},
    {".xml", "application/xml"},
    {".txt", "text/plain"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".pdf", "application/pdf"},
    {".zip", "application/zip"},
    {".tar", "application/x-tar"},
    {".gz", "application/gzip"},
    {".mp3", "audio/mpeg"},
    {".mp4", "video/mp4"},
    {".avi", "video/x-msvideo"},
    {".mov", "video/quicktime"}
};

// Logger implementation
Logger::Logger()
{
    logger_ = spdlog::default_logger();
    logger_->set_level(spdlog::level::info);
}

Logger &Logger::instance()
{
    static Logger instance;
    return instance;
}

void Logger::info(const std::string &message)
{
    logger_->info(message);
}

void Logger::warn(const std::string &message)
{
    logger_->warn(message);
}

void Logger::error(const std::string &message)
{
    logger_->error(message);
}

void Logger::debug(const std::string &message)
{
    logger_->debug(message);
}

// Request implementation
Request::Request(const http::request &req)
    : method_(req.method), path_(req.path), body_(req.body)
{
    // Convert std::map to std::unordered_map
    for (const auto &[key, value] : req.headers) {
        headers_[key] = value;
    }
    parse_query_string();
}

std::string Request::header(const std::string &name) const
{
    auto it = headers_.find(name);
    return it != headers_.end() ? it->second : "";
}

std::string Request::query(const std::string &name) const
{
    auto it = query_params_.find(name);
    return it != query_params_.end() ? it->second : "";
}

std::string Request::param(const std::string &name) const
{
    auto it = route_params_.find(name);
    return it != route_params_.end() ? it->second : "";
}

void Request::set_param(const std::string &name, const std::string &value)
{
    route_params_[name] = value;
}

bool Request::is_json() const
{
    std::string content_type = header("Content-Type");
    std::transform(content_type.begin(), content_type.end(), content_type.begin(), ::tolower);
    return content_type.find("application/json") != std::string::npos;
}

Json Request::json() const
{
    if (!json_parsed_) {
        try {
            json_cache_ = Json::parse(body_);
            json_parsed_ = true;
        } catch (const Json::parse_error &e) {
            Logger::instance().error("JSON parse error: " + std::string(e.what()));
            json_cache_ = Json{};
            json_parsed_ = true;
        }
    }
    return json_cache_;
}

std::unordered_map<std::string, std::string> Request::form() const
{
    std::unordered_map<std::string, std::string> form_data;
    
    std::string content_type = header("Content-Type");
    std::transform(content_type.begin(), content_type.end(), content_type.begin(), ::tolower);
    
    if (content_type.find("application/x-www-form-urlencoded") != std::string::npos) {
        form_data = utils::parse_query_string(body_);
    }
    
    return form_data;
}

bool Request::has_file(const std::string &field) const
{
    // Basic multipart detection - would need proper multipart parser for full implementation
    std::string content_type = header("Content-Type");
    return content_type.find("multipart/form-data") != std::string::npos && 
           body_.find("name=\"" + field + "\"") != std::string::npos;
}

void Request::parse_query_string()
{
    size_t query_pos = path_.find('?');
    if (query_pos != std::string::npos) {
        std::string query_string = path_.substr(query_pos + 1);
        path_ = path_.substr(0, query_pos);
        query_params_ = utils::parse_query_string(query_string);
    }
}

// Response implementation
Response::Response() : status_(200)
{
    headers_["Content-Type"] = "text/plain";
}

Response &Response::status(int code)
{
    status_ = code;
    return *this;
}

Response &Response::header(const std::string &name, const std::string &value)
{
    headers_[name] = value;
    return *this;
}

Response &Response::headers(const std::unordered_map<std::string, std::string> &headers)
{
    for (const auto &[key, value] : headers) {
        headers_[key] = value;
    }
    return *this;
}

Response &Response::text(const std::string &content)
{
    header("Content-Type", "text/plain");
    body_ = content;
    return *this;
}

Response &Response::html(const std::string &content)
{
    header("Content-Type", "text/html");
    body_ = content;
    return *this;
}

Response &Response::json(const Json &data)
{
    header("Content-Type", "application/json");
    body_ = data.dump();
    return *this;
}

Response &Response::file(const std::string &filepath)
{
    try {
        if (!utils::file_exists(filepath)) {
            return not_found("File not found: " + filepath);
        }
        
        body_ = utils::read_file(filepath);
        header("Content-Type", utils::mime_type(filepath));
        header("Content-Length", std::to_string(body_.size()));
        
        Logger::instance().debug("Serving file: " + filepath + " (" + std::to_string(body_.size()) + " bytes)");
    } catch (const std::exception &e) {
        Logger::instance().error("Error reading file " + filepath + ": " + e.what());
        return internal_error("Error reading file");
    }
    return *this;
}

Response &Response::send(const std::string &content)
{
    body_ = content;
    return *this;
}

Response &Response::ok(const std::string &content)
{
    status(200);
    if (!content.empty()) {
        send(content);
    }
    return *this;
}

Response &Response::created(const Json &data)
{
    status(201);
    if (!data.is_null()) {
        json(data);
    }
    return *this;
}

Response &Response::bad_request(const std::string &message)
{
    status(400).text(message);
    return *this;
}

Response &Response::unauthorized(const std::string &message)
{
    status(401).text(message);
    return *this;
}

Response &Response::forbidden(const std::string &message)
{
    status(403).text(message);
    return *this;
}

Response &Response::not_found(const std::string &message)
{
    status(404).text(message);
    return *this;
}

Response &Response::internal_error(const std::string &message)
{
    status(500).text(message);
    return *this;
}

Response &Response::redirect(const std::string &url, int code)
{
    status(code).header("Location", url);
    return *this;
}

Response &Response::cookie(const std::string &name, const std::string &value,
                          const std::string &path, int max_age)
{
    std::string cookie_value = name + "=" + value + "; Path=" + path;
    if (max_age > 0) {
        cookie_value += "; Max-Age=" + std::to_string(max_age);
    }
    header("Set-Cookie", cookie_value);
    return *this;
}

http::response Response::to_http_response() const
{
    http::response res;
    res.status = status_;
    for (const auto &[key, value] : headers_) {
        res.headers[key] = value;
    }
    res.body = body_;
    return res;
}

// SwiftNet implementation
SwiftNet::SwiftNet(uint16_t port) 
    : port_(port), threads_(std::thread::hardware_concurrency()), 
      backlog_(1024), running_(false) 
{
    Logger::instance().info("SwiftNet v1.0.0 initialized on port " + std::to_string(port));
}

SwiftNet::~SwiftNet()
{
    try {
        close();
    } catch (...) {
        // Ignore exceptions during destruction
        Logger::instance().error("Exception during SwiftNet destruction");
    }
}

SwiftNet &SwiftNet::get(const std::string &path, handler_t handler)
{
    routes_.push_back(create_route("GET", path, handler));
    return *this;
}

SwiftNet &SwiftNet::post(const std::string &path, handler_t handler)
{
    routes_.push_back(create_route("POST", path, handler));
    return *this;
}

SwiftNet &SwiftNet::put(const std::string &path, handler_t handler)
{
    routes_.push_back(create_route("PUT", path, handler));
    return *this;
}

SwiftNet &SwiftNet::del(const std::string &path, handler_t handler)
{
    routes_.push_back(create_route("DELETE", path, handler));
    return *this;
}

SwiftNet &SwiftNet::patch(const std::string &path, handler_t handler)
{
    routes_.push_back(create_route("PATCH", path, handler));
    return *this;
}

SwiftNet &SwiftNet::options(const std::string &path, handler_t handler)
{
    routes_.push_back(create_route("OPTIONS", path, handler));
    return *this;
}

SwiftNet &SwiftNet::head(const std::string &path, handler_t handler)
{
    routes_.push_back(create_route("HEAD", path, handler));
    return *this;
}

SwiftNet &SwiftNet::use(middleware_t middleware)
{
    middlewares_.push_back(middleware);
    return *this;
}

SwiftNet &SwiftNet::use(const std::string &path, middleware_t middleware)
{
    path_middlewares_.push_back({path, middleware});
    return *this;
}

SwiftNet &SwiftNet::static_files(const std::string &path, const std::string &root)
{
    return get(path + "/*", [root, path](Request &req, Response &res) {
        std::string relative_path = req.path().substr(path.length());
        if (relative_path.empty() || relative_path[0] != '/') {
            relative_path = "/" + relative_path;
        }
        
        std::string filepath = root + relative_path;
        
        // Security: prevent directory traversal
        if (filepath.find("..") != std::string::npos) {
            res.forbidden("Directory traversal not allowed");
            return;
        }
        
        if (utils::file_exists(filepath)) {
            res.file(filepath);
        } else {
            res.not_found("File not found");
        }
    });
}

SwiftNet &SwiftNet::cors(const std::string &origin)
{
    return use([origin](Request &req, Response &res, std::function<void()> next) {
        res.header("Access-Control-Allow-Origin", origin)
           .header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, PATCH")
           .header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
        
        if (req.method() == "OPTIONS") {
            res.status(200).send("");
        } else {
            next();
        }
    });
}

SwiftNet &SwiftNet::json(size_t limit)
{
    return use([limit](Request &req, Response &res, std::function<void()> next) {
        if (req.body().size() > limit) {
            res.status(413).text("Payload too large");
            return;
        }
        next();
    });
}

SwiftNet &SwiftNet::logger()
{
    return use([](Request &req, Response &res, std::function<void()> next) {
        auto start = std::chrono::high_resolution_clock::now();
        
        next();
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        Logger::instance().info(
            req.method() + " " + req.path() + " " + 
            std::to_string(res.status()) + " " + 
            std::to_string(duration.count()) + "ms"
        );
    });
}

void SwiftNet::listen(std::function<void()> callback)
{
    listen(port_, callback);
}

void SwiftNet::listen(uint16_t port, std::function<void()> callback)
{
    if (running_) return;
    
    port_ = port;
    running_ = true;
    
    std::cout << "[DEBUG] SwiftNet::listen() called on port " << port << std::endl;
    
    try {
        std::cout << "[DEBUG] Creating HTTP server..." << std::endl;
        server_ = std::make_unique<http::server>(port_, backlog_);
        std::cout << "[DEBUG] HTTP server created successfully" << std::endl;
        
        // Set up a single catch-all request handler that routes to SwiftNet
        auto handler = [this](const http::request &req, http::response &res) {
            handle_request(req, res);
        };
        
        // Register the handler for all methods and paths
        // Use a special catch-all route pattern
        std::cout << "[DEBUG] Registering catch-all route handler..." << std::endl;
        server_->route("*", "*", handler);
        
        std::cout << "[DEBUG] Starting server with " << threads_ << " threads..." << std::endl;
        server_->start(threads_);
        std::cout << "[DEBUG] Server started successfully" << std::endl;
        
        Logger::instance().info("SwiftNet server listening on port " + std::to_string(port_) + 
                               " with " + std::to_string(threads_) + " threads");
        
        if (callback) {
            callback();
        }
        
        // Print some additional info about the sophisticated virtual thread system
        std::cout << "SwiftNet advanced server listening on port " << port_ << std::endl;
        std::cout << "Virtual thread scheduler is online with sophisticated I/O handling" << std::endl;
        
        // Block until shutdown is requested
        std::unique_lock<std::mutex> lock(shutdown_mutex_);
        shutdown_cv_.wait(lock, [this]() { return shutdown_requested_; });
        
    } catch (const std::exception &e) {
        std::cout << "[DEBUG] Exception in listen(): " << e.what() << std::endl;
        Logger::instance().error("Failed to start server: " + std::string(e.what()));
        running_ = false;
        throw;
    }
}

void SwiftNet::close()
{
    if (!running_) return; // Already closed
    
    running_ = false;
    
    // Signal shutdown to wake up the blocking listen method
    {
        std::lock_guard<std::mutex> lock(shutdown_mutex_);
        shutdown_requested_ = true;
    }
    shutdown_cv_.notify_all();
    
    if (server_) {
        try {
            Logger::instance().info("Shutting down SwiftNet server");
            server_->stop();
            server_.reset();
        } catch (const std::exception &e) {
            Logger::instance().error("Error during server shutdown: " + std::string(e.what()));
        }
    }
}

SwiftNet &SwiftNet::set_threads(size_t threads)
{
    threads_ = threads > 0 ? threads : 1;
    return *this;
}

SwiftNet &SwiftNet::set_backlog(int backlog)
{
    backlog_ = backlog;
    return *this;
}

void SwiftNet::handle_request(const http::request &req, http::response &res)
{
    Request request(req);
    Response response;
    
    // Find matching route
    handler_t route_handler = nullptr;
    for (const auto &route : routes_) {
        if (match_route(route, req.method, req.path, request)) {
            route_handler = route.handler;
            break;
        }
    }
    
    if (route_handler) {
        try {
            apply_middlewares(request, response, route_handler);
        } catch (const std::exception &e) {
            Logger::instance().error("Handler error: " + std::string(e.what()));
            response.internal_error("Internal server error");
        }
    } else {
        response.not_found("Route not found: " + req.method + " " + req.path);
    }
    
    res = response.to_http_response();
}

bool SwiftNet::match_route(const Route &route, const std::string &method, 
                          const std::string &path, Request &request)
{
    if (route.method != method) return false;
    
    std::smatch matches;
    if (!std::regex_match(path, matches, route.regex)) return false;
    
    // Extract parameters
    for (size_t i = 0; i < route.param_names.size() && i + 1 < matches.size(); ++i) {
        request.set_param(route.param_names[i], matches[i + 1].str());
    }
    
    return true;
}

Route SwiftNet::create_route(const std::string &method, const std::string &pattern, handler_t handler)
{
    Route route;
    route.method = method;
    route.pattern = pattern;
    route.handler = handler;
    
    // Convert Express-style patterns to regex
    std::string regex_pattern = pattern;
    std::regex param_regex(R"(:([\w]+))");
    std::smatch matches;
    
    size_t offset = 0;
    while (std::regex_search(regex_pattern.cbegin() + offset, regex_pattern.cend(), matches, param_regex)) {
        route.param_names.push_back(matches[1].str());
        size_t pos = offset + matches.position();
        regex_pattern.replace(pos, matches.length(), "([^/]+)");
        offset = pos + 7; // length of "([^/]+)"
    }
    
    // Handle wildcards
    std::replace(regex_pattern.begin(), regex_pattern.end(), '*', '.');
    if (regex_pattern.back() != '*') {
        regex_pattern += "$";  // Exact match
    } else {
        regex_pattern.pop_back();  // Remove the '.' we just added
        regex_pattern += "*";      // Keep the wildcard
    }
    
    // Add start anchor
    if (regex_pattern.front() != '^') {
        regex_pattern = "^" + regex_pattern;
    }
    
    try {
        route.regex = std::regex(regex_pattern);
    } catch (const std::regex_error &e) {
        Logger::instance().error("Invalid regex pattern for route " + pattern + ": " + e.what());
        // Fallback to exact match
        route.regex = std::regex("^" + std::regex_replace(pattern, std::regex(R"([.*+?^${}()|[\]\\])"), R"(\$&)") + "$");
    }
    
    return route;
}

void SwiftNet::apply_middlewares(Request &req, Response &res, handler_t final_handler)
{
    std::vector<middleware_t> applicable_middlewares;
    
    // Add global middlewares
    applicable_middlewares.insert(applicable_middlewares.end(), 
                                 middlewares_.begin(), middlewares_.end());
    
    // Add path-specific middlewares
    for (const auto &[path, middleware] : path_middlewares_) {
        if (req.path().find(path) == 0) {
            applicable_middlewares.push_back(middleware);
        }
    }
    
    // Execute middleware chain
    size_t index = 0;
    std::function<void()> next = [&]() {
        if (index < applicable_middlewares.size()) {
            applicable_middlewares[index++](req, res, next);
        } else {
            final_handler(req, res);
        }
    };
    
    next();
}

// Utility functions
namespace swiftnet::utils
{
    std::string url_decode(const std::string &str)
    {
        std::string result;
        result.reserve(str.length());
        
        for (size_t i = 0; i < str.length(); ++i) {
            if (str[i] == '%' && i + 2 < str.length()) {
                try {
                    int value = std::stoi(str.substr(i + 1, 2), nullptr, 16);
                    result += static_cast<char>(value);
                    i += 2;
                } catch (...) {
                    result += str[i];
                }
            } else if (str[i] == '+') {
                result += ' ';
            } else {
                result += str[i];
            }
        }
        return result;
    }
    
    std::string url_encode(const std::string &str)
    {
        std::ostringstream oss;
        for (unsigned char c : str) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                oss << c;
            } else {
                oss << '%' << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << static_cast<int>(c);
            }
        }
        return oss.str();
    }
    
    std::unordered_map<std::string, std::string> parse_query_string(const std::string &query)
    {
        std::unordered_map<std::string, std::string> result;
        std::istringstream iss(query);
        std::string pair;
        
        while (std::getline(iss, pair, '&')) {
            size_t pos = pair.find('=');
            if (pos != std::string::npos) {
                std::string key = url_decode(pair.substr(0, pos));
                std::string value = url_decode(pair.substr(pos + 1));
                result[key] = value;
            } else {
                result[url_decode(pair)] = "";
            }
        }
        
        return result;
    }
    
    std::string mime_type(const std::string &filepath)
    {
        size_t pos = filepath.find_last_of('.');
        if (pos == std::string::npos) {
            return "application/octet-stream";
        }
        
        std::string ext = filepath.substr(pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        auto it = MIME_TYPES.find(ext);
        return it != MIME_TYPES.end() ? it->second : "application/octet-stream";
    }
    
    std::string read_file(const std::string &filepath)
    {
        std::ifstream file(filepath, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Cannot open file: " + filepath);
        }
        
        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        std::string content(size, '\0');
        file.read(&content[0], size);
        
        return content;
    }
    
    bool file_exists(const std::string &filepath)
    {
        return std::filesystem::exists(filepath) && std::filesystem::is_regular_file(filepath);
    }
    
    size_t file_size(const std::string &filepath)
    {
        try {
            return std::filesystem::file_size(filepath);
        } catch (...) {
            return 0;
        }
    }
    
    Json parse_json(const std::string &str)
    {
        try {
            return Json::parse(str);
        } catch (const Json::parse_error &e) {
            Logger::instance().error("JSON parse error: " + std::string(e.what()));
            return Json{};
        }
    }
    
    std::string serialize_json(const Json &json)
    {
        return json.dump();
    }
}

// WebSocket implementation stub
namespace swiftnet::ws
{
    WebSocketServer::WebSocketServer(SwiftNet &app) : app_(app) {}
    
    void WebSocketServer::on_connection(ws_handler_t handler)
    {
        connection_handler_ = handler;
    }
    
    void WebSocketServer::on_message(ws_message_handler_t handler)
    {
        message_handler_ = handler;
    }
    
    void WebSocketServer::on_close(ws_close_handler_t handler)
    {
        close_handler_ = handler;
    }
} 