#include "http_kv_service.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <span>

#include <glog/logging.h>

namespace mooncake::tools {

namespace {

/// @brief Default compression factor for t-digest.
constexpr double kDefaultCompression = 200.0;

/// @brief Default memory pool size (256MB).
constexpr std::size_t kDefaultMemPoolSize = 256 * 1024 * 1024;

/// @brief Default local buffer size (256MB).
constexpr std::size_t kDefaultLocalBufferSize = 256 * 1024 * 1024;

// Matches mooncake-store/src/real_client_main.cpp and
// tools/mooncake_dummy_test_service HttpTestService::DeriveIpcSocketPath.
std::string DeriveIpcSocketPath(const std::string& real_client_address,
                                const std::string& configured_ipc_socket_path) {
    if (!configured_ipc_socket_path.empty()) {
        return configured_ipc_socket_path;
    }

    const auto colon_pos = real_client_address.rfind(':');
    if (colon_pos == std::string::npos ||
        colon_pos + 1 >= real_client_address.size()) {
        throw std::invalid_argument(
            "real_client_address must be in host:port format when "
            "ipc_socket_path is not provided");
    }

    return "@mooncake_client_" + real_client_address.substr(colon_pos + 1) +
           ".sock";
}

/// @brief Result of JSON field parsing.
enum class JsonFieldStatus {
    kNotFound,       ///< Field does not exist
    kTypeMismatch,   ///< Field exists but type is wrong
    kOk              ///< Field exists and type is correct
};

/// @brief Gets a string value from JSON and reports parsing status.
/// @param j JSON object
/// @param key Field name
/// @param status Output parameter for parsing status
/// @return The string value if successful, empty string otherwise
std::string GetJsonString(const nlohmann::json& j, const char* key,
                          JsonFieldStatus& status) {
    const auto it = j.find(key);
    if (it == j.end()) {
        status = JsonFieldStatus::kNotFound;
        return {};
    }
    if (!it->is_string()) {
        status = JsonFieldStatus::kTypeMismatch;
        return {};
    }
    status = JsonFieldStatus::kOk;
    return it->get<std::string>();
}

/// @brief Gets a string value from JSON (legacy overload, returns empty string on error).
/// @param j JSON object
/// @param key Field name
/// @param default_v Default value if field not found or type mismatch
/// @return The string value if successful, default_v otherwise
std::string GetJsonString(const nlohmann::json& j, const char* key,
                          const std::string& default_v = {}) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return default_v;
    }
    return it->get<std::string>();
}

double ParseRequestSizeMb(const nlohmann::json& j, double default_mb) {
    const auto it = j.find("size");
    if (it == j.end() || it->is_null()) {
        return default_mb;
    }
    if (it->is_number_integer() || it->is_number_float()) {
        return it->get<double>();
    }
    if (it->is_string()) {
        return std::stod(it->get<std::string>());
    }
    throw std::invalid_argument("size must be a number or a numeric string");
}

std::string BoundErrorMessage(double min_mb, double max_mb) {
    return "size must be between " + std::to_string(min_mb) + " and " +
           std::to_string(max_mb) + " MB (inclusive)";
}

}  // namespace

HttpKVService::HttpKVService(std::string http_host, std::uint16_t http_port,
                             const std::string& real_client_address,
                             const std::string& ipc_socket_path,
                             std::size_t mem_pool_size,
                             std::size_t local_buffer_size,
                             double min_put_size_mb, double max_put_size_mb)
    : http_host_(std::move(http_host)),
      http_port_(http_port),
      real_client_address_(real_client_address),
      ipc_socket_path_(
          DeriveIpcSocketPath(real_client_address, ipc_socket_path)),
      mem_pool_size_(mem_pool_size > 0 ? mem_pool_size : kDefaultMemPoolSize),
      local_buffer_size_(
          local_buffer_size > 0 ? local_buffer_size : kDefaultLocalBufferSize),
      min_put_size_mb_(min_put_size_mb),
      max_put_size_mb_(max_put_size_mb),
      client_(std::make_unique<KvStoreClient>()),
      perf_collector_(std::make_unique<PerfCollector>(kDefaultCompression)),
      server_(1, http_port_, http_host_) {}

HttpKVService::~HttpKVService() { Stop(); }

bool HttpKVService::Start(std::string& error_message) {
    const int rc = client_->Setup(mem_pool_size_, local_buffer_size_,
                                  real_client_address_, ipc_socket_path_);
    if (rc != 0) {
        error_message = "Failed to setup KV store client with error code: " +
                        std::to_string(rc);
        LOG(ERROR) << error_message;
        return false;
    }

    try {
        InitRandomValueCache();
    } catch (const std::exception& e) {
        error_message = std::string("Failed to init random value cache: ") +
                        e.what();
        LOG(ERROR) << error_message;
        return false;
    }

    RegisterRoutes();
    server_.set_max_http_body_size(
        static_cast<int64_t>(std::numeric_limits<int32_t>::max()));

    auto ec = server_.async_start();
    ec.wait();
    const auto start_error = ec.value();
    if (start_error) {
        error_message = start_error.message();
        LOG(ERROR) << "Failed to start HTTP server: " << error_message;
        return false;
    }

    running_ = true;
    LOG(INFO) << "HTTP KV service started on " << http_host_ << ":" << http_port_
              << ", real_client_address=" << real_client_address_
              << ", ipc_socket_path=" << ipc_socket_path_
              << ", put size bounds [" << min_put_size_mb_ << ", "
              << max_put_size_mb_ << "] MB";
    return true;
}

void HttpKVService::Stop() {
    if (!running_) {
        return;
    }

    server_.stop();
    running_ = false;
    LOG(INFO) << "HTTP KV service stopped";
}

void HttpKVService::InitRandomValueCache() {
    // Compute byte size using the same rounding logic as request handling.
    max_value_bytes_ = static_cast<std::size_t>(
        std::llround(max_put_size_mb_ * 1024.0 * 1024.0));

    if (max_value_bytes_ == 0) {
        throw std::invalid_argument("max_put_size_mb rounds to zero bytes");
    }

    random_value_cache_.resize(max_value_bytes_);

    // Generate once at startup; subsequent requests only take read-only
    // prefixes from this buffer.
    std::mt19937_64 gen(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<> dist(0, 255);

    for (std::size_t i = 0; i < max_value_bytes_; ++i) {
        random_value_cache_[i] = static_cast<char>(dist(gen));
    }

    LOG(INFO) << "Random value cache initialized: " << max_value_bytes_
              << " bytes";
}

std::span<const char> HttpKVService::RandomValuePrefix(std::size_t size) const {
    if (size == 0) {
        return {};
    }
    if (size > random_value_cache_.size()) {
        throw std::out_of_range("RandomValuePrefix size exceeds cache size");
    }
    return std::span<const char>(random_value_cache_.data(), size);
}

void HttpKVService::RegisterRoutes() {
    using namespace coro_http;

    server_.set_http_handler<GET>(
        "/health",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleHealth(req, resp);
        });

    server_.set_http_handler<POST>(
        "/api/kv",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleKVOperation(req, resp);
        });

    server_.set_http_handler<GET>(
        "/api/performance",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandlePerformance(req, resp);
        });

    server_.set_http_handler<POST>(
        "/api/reset",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleReset(req, resp);
        });
}

void HttpKVService::HandleHealth(coro_http::coro_http_request&,
                                 coro_http::coro_http_response& resp) {
    const int health_code = client_->HealthCheck();

    nlohmann::json body;
    body["ready"] = (health_code == mooncake::HC_HEALTHY);
    body["status"] = [health_code]() {
        switch (health_code) {
            case mooncake::HC_HEALTHY:
                return "healthy";
            case mooncake::HC_NOT_INITIALIZED:
                return "not_initialized";
            case mooncake::HC_MASTER_UNREACHABLE:
                return "master_unreachable";
            default:
                return "unknown";
        }
    }();
    body["real_client_address"] = real_client_address_;
    body["ipc_socket_path"] = ipc_socket_path_;

    WriteJson(resp,
              health_code == mooncake::HC_HEALTHY
                  ? coro_http::status_type::ok
                  : coro_http::status_type::service_unavailable,
              body);
}

void HttpKVService::HandleKVOperation(coro_http::coro_http_request& req,
                                      coro_http::coro_http_response& resp) {
    const auto body_view = req.get_body();
    if (body_view.empty()) {
        WriteError(resp, coro_http::status_type::bad_request,
                   "Request body must not be empty");
        return;
    }

    nlohmann::json json_body;
    try {
        json_body = nlohmann::json::parse(body_view);
    } catch (const nlohmann::json::exception& e) {
        WriteError(resp, coro_http::status_type::bad_request,
                   std::string("Invalid JSON: ") + e.what());
        return;
    }

    JsonFieldStatus operator_status;
    const auto operator_type = GetJsonString(json_body, "operator", operator_status);
    if (operator_status == JsonFieldStatus::kNotFound) {
        WriteError(resp, coro_http::status_type::bad_request,
                   "operator is required");
        return;
    }
    if (operator_status == JsonFieldStatus::kTypeMismatch) {
        WriteError(resp, coro_http::status_type::bad_request,
                   "operator must be a string");
        return;
    }
    if (operator_type.empty()) {
        WriteError(resp, coro_http::status_type::bad_request,
                   "operator must not be empty");
        return;
    }

    PerfTimer timer;
    int result_code = 0;

    if (operator_type == "put") {
        JsonFieldStatus key_status;
        const auto key = GetJsonString(json_body, "key", key_status);
        if (key_status == JsonFieldStatus::kNotFound || key_status == JsonFieldStatus::kTypeMismatch) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key must be a string");
            return;
        }
        if (key.empty()) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key must not be empty");
            return;
        }

        double size_mb = 0.0;
        try {
            size_mb = ParseRequestSizeMb(json_body, 1.0);
        } catch (const std::exception& e) {
            WriteError(resp, coro_http::status_type::bad_request,
                       std::string("Invalid size: ") + e.what());
            return;
        }

        if (size_mb < min_put_size_mb_ || size_mb > max_put_size_mb_) {
            WriteError(resp, coro_http::status_type::bad_request,
                       BoundErrorMessage(min_put_size_mb_, max_put_size_mb_));
            return;
        }

        const std::size_t value_bytes = static_cast<std::size_t>(
            std::llround(size_mb * 1024.0 * 1024.0));
        if (value_bytes == 0) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "size rounds to zero bytes");
            return;
        }

        const auto value_span = RandomValuePrefix(value_bytes);
        result_code = client_->Put(key, value_span);

        perf_collector_->Record("put", timer.ElapsedMs());

        nlohmann::json result;
        result["operator"] = operator_type;
        result["key"] = key;
        result["code"] = result_code;
        result["duration_ms"] = timer.ElapsedMs();
        result["size_mb"] = size_mb;
        result["value_bytes"] = value_bytes;
        WriteJson(resp, coro_http::status_type::ok, result);

    } else if (operator_type == "get") {
        JsonFieldStatus key_status;
        const auto key = GetJsonString(json_body, "key", key_status);
        if (key_status == JsonFieldStatus::kNotFound || key_status == JsonFieldStatus::kTypeMismatch) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key must be a string");
            return;
        }
        if (key.empty()) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key must not be empty");
            return;
        }

        std::string value;
        result_code = client_->Get(key, value);

        perf_collector_->Record("get", timer.ElapsedMs());

        nlohmann::json result;
        result["operator"] = operator_type;
        result["key"] = key;
        result["code"] = result_code;
        result["size"] = value.size();
        result["duration_ms"] = timer.ElapsedMs();
        WriteJson(resp, coro_http::status_type::ok, result);

    } else if (operator_type == "exist") {
        JsonFieldStatus key_status;
        const auto key = GetJsonString(json_body, "key", key_status);
        if (key_status == JsonFieldStatus::kNotFound || key_status == JsonFieldStatus::kTypeMismatch) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key must be a string");
            return;
        }
        if (key.empty()) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key must not be empty");
            return;
        }

        const int exists = client_->Exist(key);

        perf_collector_->Record("exist", timer.ElapsedMs());

        nlohmann::json result;
        result["operator"] = operator_type;
        result["key"] = key;
        result["exists"] = (exists == 1);
        result["code"] = exists;
        result["duration_ms"] = timer.ElapsedMs();
        WriteJson(resp, coro_http::status_type::ok, result);

    } else if (operator_type == "batch_put") {
        JsonFieldStatus keys_str_status;
        const auto keys_str = GetJsonString(json_body, "key", keys_str_status);
        if (keys_str_status == JsonFieldStatus::kNotFound || keys_str_status == JsonFieldStatus::kTypeMismatch) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key (comma-separated string) is required for batch_put");
            return;
        }
        if (keys_str.empty()) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key must not be empty");
            return;
        }

        double size_mb = 0.0;
        try {
            size_mb = ParseRequestSizeMb(json_body, 1.0);
        } catch (const std::exception& e) {
            WriteError(resp, coro_http::status_type::bad_request,
                       std::string("Invalid size: ") + e.what());
            return;
        }

        if (size_mb < min_put_size_mb_ || size_mb > max_put_size_mb_) {
            WriteError(resp, coro_http::status_type::bad_request,
                       BoundErrorMessage(min_put_size_mb_, max_put_size_mb_));
            return;
        }

        const std::size_t value_bytes = static_cast<std::size_t>(
            std::llround(size_mb * 1024.0 * 1024.0));
        if (value_bytes == 0) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "size rounds to zero bytes");
            return;
        }

        const auto keys = ParseKeys(keys_str);
        const auto value_span = RandomValuePrefix(value_bytes);
        const auto results = client_->BatchPut(keys, value_span);

        perf_collector_->Record("batch_put", timer.ElapsedMs());

        nlohmann::json result;
        result["operator"] = operator_type;
        result["keys"] = nlohmann::json::array();
        result["total"] = keys.size();
        result["success"] = static_cast<std::size_t>(
            std::count(results.begin(), results.end(), 0));
        result["duration_ms"] = timer.ElapsedMs();
        result["size_mb"] = size_mb;
        result["value_bytes"] = value_bytes;
        for (const auto& k : keys) {
            result["keys"].push_back(k);
        }
        WriteJson(resp, coro_http::status_type::ok, result);

    } else if (operator_type == "batch_get") {
        JsonFieldStatus keys_str_status;
        const auto keys_str = GetJsonString(json_body, "key", keys_str_status);
        if (keys_str_status == JsonFieldStatus::kNotFound || keys_str_status == JsonFieldStatus::kTypeMismatch) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key (comma-separated string) is required for batch_get");
            return;
        }
        if (keys_str.empty()) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key must not be empty");
            return;
        }

        const auto keys = ParseKeys(keys_str);
        std::vector<std::string> values;
        const auto results = client_->BatchGet(keys, values);

        perf_collector_->Record("batch_get", timer.ElapsedMs());

        nlohmann::json result;
        result["operator"] = operator_type;
        result["keys"] = nlohmann::json::array();
        result["total"] = keys.size();
        result["success"] = static_cast<std::size_t>(
            std::count(results.begin(), results.end(), 0));
        result["duration_ms"] = timer.ElapsedMs();
        for (const auto& k : keys) {
            result["keys"].push_back(k);
        }
        WriteJson(resp, coro_http::status_type::ok, result);

    } else if (operator_type == "batch_exist") {
        JsonFieldStatus keys_str_status;
        const auto keys_str = GetJsonString(json_body, "key", keys_str_status);
        if (keys_str_status == JsonFieldStatus::kNotFound || keys_str_status == JsonFieldStatus::kTypeMismatch) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key (comma-separated string) is required for batch_exist");
            return;
        }
        if (keys_str.empty()) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "key must not be empty");
            return;
        }

        const auto keys = ParseKeys(keys_str);
        const auto results = client_->BatchExist(keys);

        perf_collector_->Record("batch_exist", timer.ElapsedMs());

        nlohmann::json result;
        result["operator"] = operator_type;
        result["keys"] = nlohmann::json::array();
        result["total"] = keys.size();
        result["exists"] = static_cast<std::size_t>(
            std::count(results.begin(), results.end(), 1));
        result["duration_ms"] = timer.ElapsedMs();
        for (const auto& k : keys) {
            result["keys"].push_back(k);
        }
        WriteJson(resp, coro_http::status_type::ok, result);

    } else {
        WriteError(resp, coro_http::status_type::bad_request,
                   "Unknown operator: " + operator_type);
    }
}

void HttpKVService::HandlePerformance(coro_http::coro_http_request&,
                                      coro_http::coro_http_response& resp) {
    const auto reports = perf_collector_->GetReports();

    nlohmann::json result = nlohmann::json::array();
    for (const auto& report : reports) {
        result.push_back(ReportToJson(report));
    }

    WriteJson(resp, coro_http::status_type::ok, result);
}

void HttpKVService::HandleReset(coro_http::coro_http_request&,
                                coro_http::coro_http_response& resp) {
    perf_collector_->Reset();

    nlohmann::json result;
    result["status"] = "reset_success";
    result["message"] = "Performance data has been reset";

    WriteJson(resp, coro_http::status_type::ok, result);
}

nlohmann::json HttpKVService::ReportToJson(const PerfCollector::Report& report) {
    nlohmann::json root;
    root["tag"] = report.tag;
    root["avgMs"] = report.avg_ms;
    root["minMs"] = report.min_ms;
    root["maxMs"] = report.max_ms;
    root["p90Ms"] = report.p90_ms;
    root["p95Ms"] = report.p95_ms;
    root["p99Ms"] = report.p99_ms;
    root["qps"] = report.qps;
    root["totalDurationMs"] = report.total_duration_ms;
    root["totalTimeSpanMs"] = report.total_time_span_ms;
    root["totalRequests"] = report.total_requests;
    return root;
}

void HttpKVService::WriteJson(coro_http::coro_http_response& resp,
                              coro_http::status_type status,
                              const nlohmann::json& body) {
    resp.add_header("Content-Type", "application/json; charset=utf-8");
    resp.set_status_and_content(status, body.dump());
}

void HttpKVService::WriteError(coro_http::coro_http_response& resp,
                               coro_http::status_type status,
                               const std::string& message) {
    nlohmann::json body;
    body["error"] = message;
    WriteJson(resp, status, body);
}

std::vector<std::string> HttpKVService::ParseKeys(const std::string& keys_str) {
    std::vector<std::string> keys;
    std::stringstream ss(keys_str);
    std::string key;
    while (std::getline(ss, key, ',')) {
        if (!key.empty()) {
            keys.push_back(key);
        }
    }
    return keys;
}

}  // namespace mooncake::tools
