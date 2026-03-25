#pragma once

#include <atomic>
#include <csignal>
#include <cstdint>
#include <memory>
#include <string>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>
#include <ylt/coro_http/coro_http_server.hpp>

#include "kv_store_client.hpp"
#include "perf_collector.h"

namespace mooncake::tools {

/// @brief HTTP service for KVStore performance testing.
/// @note Provides RESTful API for put/get/exist operations and performance monitoring.
class HttpKVService {
   public:
    /// @brief Constructs a new HttpKVService.
    /// @param http_host HTTP listen host.
    /// @param http_port HTTP listen port.
    /// @param real_client_address Real client RPC address.
    /// @param ipc_socket_path IPC socket path.
    /// @param mem_pool_size Memory pool size in bytes.
    /// @param local_buffer_size Local buffer size in bytes.
    /// @param min_put_size_mb Minimum allowed payload size (MB) for put / batch_put.
    /// @param max_put_size_mb Maximum allowed payload size (MB) for put / batch_put.
    explicit HttpKVService(std::string http_host, std::uint16_t http_port,
                           const std::string& real_client_address,
                           const std::string& ipc_socket_path,
                           std::size_t mem_pool_size,
                           std::size_t local_buffer_size, double min_put_size_mb,
                           double max_put_size_mb);

    /// @brief Destroys the HttpKVService.
    ~HttpKVService();

    HttpKVService(const HttpKVService&) = delete;
    HttpKVService& operator=(const HttpKVService&) = delete;

    /// @brief Starts the HTTP server.
    /// @param error_message Output parameter for error message.
    /// @return true on success, false on failure.
    bool Start(std::string& error_message);

    /// @brief Stops the HTTP server.
    void Stop();

    /// @brief Checks if the server is running.
    /// @return true if running, false otherwise.
    [[nodiscard]] bool IsRunning() const { return running_; }

   private:
    /// @brief Registers all HTTP routes.
    void RegisterRoutes();

    /// @brief Handles health check requests.
    /// @param req HTTP request.
    /// @param resp HTTP response.
    void HandleHealth(coro_http::coro_http_request& req,
                      coro_http::coro_http_response& resp);

    /// @brief Handles KV store operations.
    /// @param req HTTP request.
    /// @param resp HTTP response.
    void HandleKVOperation(coro_http::coro_http_request& req,
                           coro_http::coro_http_response& resp);

    /// @brief Handles performance data requests.
    /// @param req HTTP request.
    /// @param resp HTTP response.
    void HandlePerformance(coro_http::coro_http_request& req,
                           coro_http::coro_http_response& resp);

    /// @brief Handles reset requests.
    /// @param req HTTP request.
    /// @param resp HTTP response.
    void HandleReset(coro_http::coro_http_request& req,
                     coro_http::coro_http_response& resp);

    /// @brief Writes a JSON response.
    /// @param resp HTTP response.
    /// @param status HTTP status code.
    /// @param body JSON body.
    void WriteJson(coro_http::coro_http_response& resp,
                   coro_http::status_type status, const nlohmann::json& body);

    /// @brief Writes an error response.
    /// @param resp HTTP response.
    /// @param status HTTP status code.
    /// @param message Error message.
    void WriteError(coro_http::coro_http_response& resp,
                    coro_http::status_type status, const std::string& message);

    /// @brief Converts a performance report to JSON.
    /// @param report Performance report.
    /// @return JSON value.
    static nlohmann::json ReportToJson(const PerfCollector::Report& report);

    /// @brief Parses keys from a comma-separated string.
    /// @param keys_str Comma-separated keys.
    /// @return Vector of keys.
    static std::vector<std::string> ParseKeys(const std::string& keys_str);

    /// @brief Prepares the random value cache used for put/batch_put.
    void InitRandomValueCache();

    /// @brief Gets read-only prefix view from the cached random value.
    /// @param size Number of bytes to take from the front.
    /// @return Read-only span pointing into the cache.
    [[nodiscard]] std::span<const char> RandomValuePrefix(std::size_t size) const;

    std::string http_host_;
    std::uint16_t http_port_;
    std::string real_client_address_;
    std::string ipc_socket_path_;
    std::size_t mem_pool_size_;
    std::size_t local_buffer_size_;
    double min_put_size_mb_;
    double max_put_size_mb_;

    std::size_t max_value_bytes_{0};
    std::string random_value_cache_;

    std::unique_ptr<KvStoreClient> client_;
    std::unique_ptr<PerfCollector> perf_collector_;

    coro_http::coro_http_server server_;
    std::atomic<bool> running_{false};
};

}  // namespace mooncake::tools
