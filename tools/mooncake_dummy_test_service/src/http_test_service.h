#pragma once

#include <csignal>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <json/json.h>
#include <ylt/coro_http/coro_http_server.hpp>

#include "benchmark_runner.h"
#include "dummy_client.h"

namespace mooncake::tools {

class HttpTestService {
   public:
    HttpTestService(std::string http_host, std::uint16_t http_port,
                    std::string real_client_address, std::string ipc_socket_path,
                    std::size_t mem_pool_size, std::size_t local_buffer_size,
                    std::size_t default_replica_num);
    ~HttpTestService();

    bool Start(std::string& error_message);
    void Stop();

   private:
    std::shared_ptr<mooncake::DummyClient> CreateDummyClient(
        std::string* error_message = nullptr) const;
    void RegisterRoutes();

    void HandleHealth(coro_http::coro_http_request& req,
                      coro_http::coro_http_response& resp);
    void HandlePutObject(coro_http::coro_http_request& req,
                         coro_http::coro_http_response& resp);
    void HandleGetObject(coro_http::coro_http_request& req,
                         coro_http::coro_http_response& resp);
    void HandleGetObjectMeta(coro_http::coro_http_request& req,
                             coro_http::coro_http_response& resp);
    void HandleDeleteObject(coro_http::coro_http_request& req,
                            coro_http::coro_http_response& resp);
    void HandleBenchmarkStart(coro_http::coro_http_request& req,
                              coro_http::coro_http_response& resp);
    void HandleBenchmarkStop(coro_http::coro_http_request& req,
                             coro_http::coro_http_response& resp);
    void HandleBenchmarkStatus(coro_http::coro_http_request& req,
                               coro_http::coro_http_response& resp);

    static Json::Value SnapshotToJson(const BenchmarkSnapshot& snapshot);
    static std::string SerializeJson(const Json::Value& value);
    static void WriteJson(coro_http::coro_http_response& resp,
                          coro_http::status_type status, const Json::Value& body);
    static void WriteError(coro_http::coro_http_response& resp,
                           coro_http::status_type status,
                           const std::string& message);
    static std::optional<std::string> ExtractObjectKey(std::string_view url,
                                                       bool meta_endpoint);
    static std::string UrlDecode(std::string_view encoded);
    static std::string DeriveIpcSocketPath(
        const std::string& real_client_address,
        const std::string& configured_ipc_socket_path);
    static std::optional<std::size_t> ParseSizeT(std::string_view value);
    static std::string HealthStatusToString(int health_code);

    const std::string http_host_;
    const std::uint16_t http_port_;
    const std::string real_client_address_;
    const std::string ipc_socket_path_;
    const std::size_t mem_pool_size_;
    const std::size_t local_buffer_size_;
    const std::size_t default_replica_num_;

    std::shared_ptr<mooncake::DummyClient> control_client_;
    mutable std::mutex control_client_mutex_;

    BenchmarkRunner benchmark_runner_;
    coro_http::coro_http_server server_;
    bool running_{false};
};

}  // namespace mooncake::tools
