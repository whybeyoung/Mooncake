#include "http_test_service.h"

#include <chrono>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <glog/logging.h>

#include "replica.h"
#include "types.h"

namespace mooncake {

PyClient::~PyClient() = default;

}  // namespace mooncake

namespace mooncake::tools {

namespace {

std::optional<Json::Value> ParseJsonBody(std::string_view body,
                                         std::string& error_message) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    Json::Value root;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &root, &errors)) {
        error_message = errors;
        return std::nullopt;
    }
    return root;
}

std::string_view TrimPrefix(std::string_view value, std::string_view prefix) {
    if (value.substr(0, prefix.size()) == prefix) {
        return value.substr(prefix.size());
    }
    return value;
}

}  // namespace

HttpTestService::HttpTestService(std::string http_host, std::uint16_t http_port,
                                 std::string real_client_address,
                                 std::string ipc_socket_path,
                                 std::size_t mem_pool_size,
                                 std::size_t local_buffer_size,
                                 std::size_t default_replica_num)
    : http_host_(std::move(http_host)),
      http_port_(http_port),
      real_client_address_(std::move(real_client_address)),
      ipc_socket_path_(
          DeriveIpcSocketPath(real_client_address_, ipc_socket_path)),
      mem_pool_size_(mem_pool_size),
      local_buffer_size_(local_buffer_size),
      default_replica_num_(default_replica_num),
      benchmark_runner_([this]() { return CreateDummyClient(); }),
      server_(1, http_port_, http_host_) {}

HttpTestService::~HttpTestService() { Stop(); }

bool HttpTestService::Start(std::string& error_message) {
    control_client_ = CreateDummyClient(&error_message);
    if (!control_client_) {
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
        {
            std::lock_guard<std::mutex> lock(control_client_mutex_);
            control_client_.reset();
        }
        return false;
    }

    running_ = true;
    LOG(INFO) << "Dummy HTTP test service started on " << http_host_ << ":"
              << http_port_ << ", real_client_address=" << real_client_address_
              << ", ipc_socket_path=" << ipc_socket_path_;
    return true;
}

void HttpTestService::Stop() {
    if (!running_) {
        return;
    }

    std::string error_message;
    (void)benchmark_runner_.Stop(error_message);
    server_.stop();
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        control_client_.reset();
    }
    running_ = false;
    LOG(INFO) << "Dummy HTTP test service stopped";
}

std::shared_ptr<mooncake::DummyClient> HttpTestService::CreateDummyClient(
    std::string* error_message) const {
    auto client = std::make_shared<mooncake::DummyClient>();
    const int rc = client->setup_dummy(mem_pool_size_, local_buffer_size_,
                                       real_client_address_, ipc_socket_path_);
    if (rc != 0) {
        if (error_message != nullptr) {
            *error_message = "failed to connect to realclient via DummyClient";
        }
        return nullptr;
    }
    return client;
}

void HttpTestService::RegisterRoutes() {
    using namespace coro_http;

    server_.set_http_handler<GET>(
        "/health",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleHealth(req, resp);
        });

    server_.set_http_handler<GET>(
        R"(/api/object/(.+)/meta)",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleGetObjectMeta(req, resp);
        });

    server_.set_http_handler<GET>(
        R"(/api/object/(.+)/replicas)",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleGetObjectReplicas(req, resp);
        });

    server_.set_http_handler<PUT>(
        R"(/api/object/([^/]+))",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandlePutObject(req, resp);
        });

    server_.set_http_handler<GET>(
        R"(/api/object/([^/]+))",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleGetObject(req, resp);
        });

    server_.set_http_handler<DEL>(
        R"(/api/object/([^/]+))",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleDeleteObject(req, resp);
        });

    server_.set_http_handler<GET>(
        "/api/service/config",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleServiceConfig(req, resp);
        });

    server_.set_http_handler<POST>(
        "/api/objects/replicas",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleBatchObjectReplicas(req, resp);
        });

    server_.set_http_handler<POST>(
        "/api/objects/exists",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleBatchObjectExists(req, resp);
        });

    server_.set_http_handler<POST>(
        "/api/objects/delete_by_regex",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleDeleteObjectsByRegex(req, resp);
        });

    server_.set_http_handler<POST>(
        "/api/objects/delete_all",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleDeleteAllObjects(req, resp);
        });

    server_.set_http_handler<POST>(
        "/api/benchmark/start",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleBenchmarkStart(req, resp);
        });

    server_.set_http_handler<POST>(
        "/api/benchmark/stop",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleBenchmarkStop(req, resp);
        });

    server_.set_http_handler<GET>(
        "/api/benchmark/status",
        [this](coro_http_request& req, coro_http_response& resp) {
            HandleBenchmarkStatus(req, resp);
        });
}

void HttpTestService::HandleHealth(coro_http::coro_http_request&,
                                   coro_http::coro_http_response& resp) {
    int health_code = HC_NOT_INITIALIZED;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        if (control_client_) {
            health_code = control_client_->health_check();
        }
    }

    Json::Value body(Json::objectValue);
    body["ready"] = (health_code == HC_HEALTHY);
    body["status"] = HealthStatusToString(health_code);
    body["real_client_address"] = real_client_address_;
    body["ipc_socket_path"] = ipc_socket_path_;
    WriteJson(resp, health_code == HC_HEALTHY ? coro_http::status_type::ok
                                              : coro_http::status_type::service_unavailable,
              body);
}

void HttpTestService::HandlePutObject(coro_http::coro_http_request& req,
                                      coro_http::coro_http_response& resp) {
    if (benchmark_runner_.IsActive()) {
        WriteError(resp, coro_http::status_type::conflict,
                   "benchmark is active");
        return;
    }

    auto key = ExtractObjectKey(req.get_url(), false);
    if (!key.has_value()) {
        WriteError(resp, coro_http::status_type::bad_request, "invalid key path");
        return;
    }

    const auto body_view = req.get_body();
    if (body_view.empty()) {
        WriteError(resp, coro_http::status_type::bad_request,
                   "request body must not be empty");
        return;
    }

    std::size_t replica_num = default_replica_num_;
    const auto replica_num_query = req.get_query_value("replica_num");
    if (!replica_num_query.empty()) {
        auto parsed = ParseSizeT(replica_num_query);
        if (!parsed.has_value() || *parsed == 0) {
            WriteError(resp, coro_http::status_type::bad_request,
                       "replica_num must be a positive integer");
            return;
        }
        replica_num = *parsed;
    }

    const std::string payload(body_view.data(), body_view.size());
    ReplicateConfig config;
    config.replica_num = replica_num;

    int rc = -1;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        rc = control_client_->put(*key,
                                  std::span<const char>(payload.data(),
                                                        payload.size()),
                                  config);
    }
    if (rc != 0) {
        WriteError(resp, coro_http::status_type::internal_server_error,
                   "put failed with code " + std::to_string(rc));
        return;
    }

    Json::Value body(Json::objectValue);
    body["key"] = *key;
    body["size"] = static_cast<Json::UInt64>(payload.size());
    body["code"] = rc;
    WriteJson(resp, coro_http::status_type::created, body);
}

void HttpTestService::HandleGetObject(coro_http::coro_http_request& req,
                                      coro_http::coro_http_response& resp) {
    if (benchmark_runner_.IsActive()) {
        WriteError(resp, coro_http::status_type::conflict,
                   "benchmark is active");
        return;
    }

    auto key = ExtractObjectKey(req.get_url(), false);
    if (!key.has_value()) {
        WriteError(resp, coro_http::status_type::bad_request, "invalid key path");
        return;
    }

    std::string payload;
    bool found = false;
    int exists = 0;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        auto handle = control_client_->get_buffer(*key);
        if (handle != nullptr) {
            payload.assign(static_cast<const char*>(handle->ptr()), handle->size());
            found = true;
            handle.reset();
        } else {
            exists = control_client_->isExist(*key);
        }
    }

    if (!found) {
        if (exists == 0) {
            WriteError(resp, coro_http::status_type::not_found, "object not found");
        } else {
            WriteError(resp, coro_http::status_type::internal_server_error,
                       "get failed");
        }
        return;
    }

    resp.add_header("Content-Type", "application/octet-stream");
    resp.add_header("X-Mooncake-Key", *key);
    resp.add_header("X-Mooncake-Size", std::to_string(payload.size()));
    resp.set_status_and_content(coro_http::status_type::ok, std::move(payload));
}

void HttpTestService::HandleGetObjectMeta(coro_http::coro_http_request& req,
                                          coro_http::coro_http_response& resp) {
    if (benchmark_runner_.IsActive()) {
        WriteError(resp, coro_http::status_type::conflict,
                   "benchmark is active");
        return;
    }

    auto key = ExtractObjectKey(req.get_url(), true);
    if (!key.has_value()) {
        WriteError(resp, coro_http::status_type::bad_request, "invalid key path");
        return;
    }

    int exists = -1;
    int64_t size = -1;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        exists = control_client_->isExist(*key);
        if (exists == 1) {
            size = control_client_->getSize(*key);
        }
    }

    Json::Value body(Json::objectValue);
    body["key"] = *key;
    body["exists"] = (exists == 1);
    if (size >= 0) {
        body["size"] = static_cast<Json::UInt64>(size);
    } else {
        body["size"] = Json::nullValue;
    }
    WriteJson(resp, coro_http::status_type::ok, body);
}

void HttpTestService::HandleGetObjectReplicas(coro_http::coro_http_request& req,
                                              coro_http::coro_http_response& resp) {
    if (benchmark_runner_.IsActive()) {
        WriteError(resp, coro_http::status_type::conflict,
                   "benchmark is active");
        return;
    }

    auto key = ExtractObjectKey(req.get_url(), false);
    if (!key.has_value() || key->size() < std::string("/replicas").size() ||
        key->substr(key->size() - std::string("/replicas").size()) !=
            "/replicas") {
        WriteError(resp, coro_http::status_type::bad_request,
                   "invalid key path");
        return;
    }
    key->resize(key->size() - std::string("/replicas").size());
    if (key->empty()) {
        WriteError(resp, coro_http::status_type::bad_request,
                   "invalid key path");
        return;
    }

    int exists = -1;
    std::vector<Replica::Descriptor> replicas;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        exists = control_client_->isExist(*key);
        if (exists == 1) {
            replicas = control_client_->get_replica_desc(*key);
        }
    }

    if (exists == 0) {
        WriteError(resp, coro_http::status_type::not_found, "object not found");
        return;
    }
    if (exists != 1) {
        WriteError(resp, coro_http::status_type::internal_server_error,
                   "replica query failed");
        return;
    }

    Json::Value body(Json::objectValue);
    body["key"] = *key;
    body["exists"] = true;
    body["replicas"] = ReplicaDescriptorsToJson(replicas);
    WriteJson(resp, coro_http::status_type::ok, body);
}

void HttpTestService::HandleBatchObjectReplicas(
    coro_http::coro_http_request& req, coro_http::coro_http_response& resp) {
    if (benchmark_runner_.IsActive()) {
        WriteError(resp, coro_http::status_type::conflict,
                   "benchmark is active");
        return;
    }

    std::string parse_error;
    auto keys = ParseKeysFromBody(req.get_body(), parse_error);
    if (!keys.has_value()) {
        WriteError(resp, coro_http::status_type::bad_request, parse_error);
        return;
    }

    std::vector<int> exists_results;
    std::map<std::string, std::vector<Replica::Descriptor>> replica_map;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        exists_results = control_client_->batchIsExist(*keys);
        replica_map = control_client_->batch_get_replica_desc(*keys);
    }

    Json::Value body(Json::objectValue);
    Json::Value objects(Json::objectValue);
    for (std::size_t index = 0; index < keys->size(); ++index) {
        Json::Value entry(Json::objectValue);
        const auto& key = keys->at(index);
        const int exists =
            index < exists_results.size() ? exists_results[index] : -1;
        entry["exists"] = (exists == 1);
        entry["replicas"] = exists == 1 && replica_map.count(key) > 0
                                ? ReplicaDescriptorsToJson(replica_map.at(key))
                                : Json::Value(Json::arrayValue);
        objects[key] = std::move(entry);
    }
    body["objects"] = std::move(objects);
    WriteJson(resp, coro_http::status_type::ok, body);
}

void HttpTestService::HandleBatchObjectExists(coro_http::coro_http_request& req,
                                              coro_http::coro_http_response& resp) {
    if (benchmark_runner_.IsActive()) {
        WriteError(resp, coro_http::status_type::conflict,
                   "benchmark is active");
        return;
    }

    std::string parse_error;
    auto keys = ParseKeysFromBody(req.get_body(), parse_error);
    if (!keys.has_value()) {
        WriteError(resp, coro_http::status_type::bad_request, parse_error);
        return;
    }

    std::vector<int> exists_results;
    std::vector<int64_t> sizes(keys->size(), -1);
    std::map<std::string, std::vector<Replica::Descriptor>> replica_map;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        exists_results = control_client_->batchIsExist(*keys);
        replica_map = control_client_->batch_get_replica_desc(*keys);
        for (std::size_t index = 0; index < keys->size(); ++index) {
            if (index < exists_results.size() && exists_results[index] == 1) {
                sizes[index] = control_client_->getSize(keys->at(index));
            }
        }
    }

    Json::Value body(Json::objectValue);
    Json::Value objects(Json::objectValue);
    for (std::size_t index = 0; index < keys->size(); ++index) {
        Json::Value entry(Json::objectValue);
        const auto& key = keys->at(index);
        const int exists =
            index < exists_results.size() ? exists_results[index] : -1;
        entry["exists"] = (exists == 1);
        entry["size"] = sizes[index] >= 0 ? Json::Value(static_cast<Json::UInt64>(
                                              sizes[index]))
                                          : Json::Value(Json::nullValue);
        entry["replicas"] = exists == 1 && replica_map.count(key) > 0
                                ? ReplicaDescriptorsToJson(replica_map.at(key))
                                : Json::Value(Json::arrayValue);
        objects[key] = std::move(entry);
    }
    body["objects"] = std::move(objects);
    WriteJson(resp, coro_http::status_type::ok, body);
}

void HttpTestService::HandleDeleteObject(coro_http::coro_http_request& req,
                                         coro_http::coro_http_response& resp) {
    if (benchmark_runner_.IsActive()) {
        WriteError(resp, coro_http::status_type::conflict,
                   "benchmark is active");
        return;
    }

    auto key = ExtractObjectKey(req.get_url(), false);
    if (!key.has_value()) {
        WriteError(resp, coro_http::status_type::bad_request, "invalid key path");
        return;
    }

    int rc = -1;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        rc = control_client_->remove(*key);
    }
    if (rc == toInt(ErrorCode::OBJECT_NOT_FOUND)) {
        WriteError(resp, coro_http::status_type::not_found, "object not found");
        return;
    }
    if (rc != 0) {
        WriteError(resp, coro_http::status_type::internal_server_error,
                   "remove failed with code " + std::to_string(rc));
        return;
    }

    Json::Value body(Json::objectValue);
    body["key"] = *key;
    body["code"] = rc;
    WriteJson(resp, coro_http::status_type::ok, body);
}

void HttpTestService::HandleDeleteObjectsByRegex(
    coro_http::coro_http_request& req, coro_http::coro_http_response& resp) {
    if (benchmark_runner_.IsActive()) {
        WriteError(resp, coro_http::status_type::conflict,
                   "benchmark is active");
        return;
    }

    std::string parse_error;
    auto json =
        ParseJsonBody(std::string(req.get_body()), parse_error)
            .value_or(Json::Value(Json::objectValue));
    if (!parse_error.empty()) {
        WriteError(resp, coro_http::status_type::bad_request, parse_error);
        return;
    }

    const auto regex = json.get("regex", "").asString();
    const bool force = json.get("force", false).asBool();
    if (regex.empty()) {
        WriteError(resp, coro_http::status_type::bad_request,
                   "regex must not be empty");
        return;
    }

    long removed = -1;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        removed = control_client_->removeByRegex(regex, force);
    }
    if (removed < 0) {
        WriteError(resp, coro_http::status_type::internal_server_error,
                   "removeByRegex failed");
        return;
    }

    Json::Value body(Json::objectValue);
    body["regex"] = regex;
    body["force"] = force;
    body["removed"] = static_cast<Json::Int64>(removed);
    WriteJson(resp, coro_http::status_type::ok, body);
}

void HttpTestService::HandleDeleteAllObjects(coro_http::coro_http_request& req,
                                             coro_http::coro_http_response& resp) {
    if (benchmark_runner_.IsActive()) {
        WriteError(resp, coro_http::status_type::conflict,
                   "benchmark is active");
        return;
    }

    std::string parse_error;
    auto json =
        ParseJsonBody(std::string(req.get_body()), parse_error)
            .value_or(Json::Value(Json::objectValue));
    if (!parse_error.empty()) {
        WriteError(resp, coro_http::status_type::bad_request, parse_error);
        return;
    }

    const bool force = json.get("force", false).asBool();
    long removed = -1;
    {
        std::lock_guard<std::mutex> lock(control_client_mutex_);
        removed = control_client_->removeAll(force);
    }
    if (removed < 0) {
        WriteError(resp, coro_http::status_type::internal_server_error,
                   "removeAll failed");
        return;
    }

    Json::Value body(Json::objectValue);
    body["force"] = force;
    body["removed"] = static_cast<Json::Int64>(removed);
    WriteJson(resp, coro_http::status_type::ok, body);
}

void HttpTestService::HandleServiceConfig(coro_http::coro_http_request&,
                                          coro_http::coro_http_response& resp) {
    Json::Value body(Json::objectValue);
    body["real_client_address"] = real_client_address_;
    body["ipc_socket_path"] = ipc_socket_path_;
    body["mem_pool_size"] = static_cast<Json::UInt64>(mem_pool_size_);
    body["local_buffer_size"] =
        static_cast<Json::UInt64>(local_buffer_size_);
    body["default_replica_num"] =
        static_cast<Json::UInt64>(default_replica_num_);
    WriteJson(resp, coro_http::status_type::ok, body);
}

void HttpTestService::HandleBenchmarkStart(coro_http::coro_http_request& req,
                                           coro_http::coro_http_response& resp) {
    std::string parse_error;
    auto json =
        ParseJsonBody(std::string(req.get_body()), parse_error).value_or(Json::Value(Json::objectValue));
    if (!parse_error.empty()) {
        WriteError(resp, coro_http::status_type::bad_request, parse_error);
        return;
    }

    BenchmarkConfig config;
    config.key_prefix =
        json.get("key_prefix", config.key_prefix).asString();
    config.cleanup = json.get("cleanup", true).asBool();
    config.replica_num = static_cast<std::size_t>(
        json.get("replica_num", static_cast<Json::UInt64>(default_replica_num_))
            .asUInt64());
    config.seed = json.get("seed", static_cast<Json::UInt64>(config.seed))
                      .asUInt64();
    config.object_size =
        static_cast<std::size_t>(json.get("object_size", 0).asUInt64());
    config.batch_size =
        static_cast<std::size_t>(json.get("batch_size", 0).asUInt64());
    config.iterations =
        static_cast<std::size_t>(json.get("iterations", 0).asUInt64());
    config.concurrency =
        static_cast<std::size_t>(json.get("concurrency", 0).asUInt64());

    if (config.key_prefix.empty()) {
        WriteError(resp, coro_http::status_type::bad_request,
                   "key_prefix must not be empty");
        return;
    }
    if (config.object_size == 0 || config.batch_size == 0 ||
        config.iterations == 0 || config.concurrency == 0 ||
        config.replica_num == 0) {
        WriteError(resp, coro_http::status_type::bad_request,
                   "object_size, batch_size, iterations, concurrency, "
                   "replica_num must all be greater than 0");
        return;
    }

    std::string error_message;
    if (!benchmark_runner_.Start(config, error_message)) {
        WriteError(resp, coro_http::status_type::conflict, error_message);
        return;
    }

    WriteJson(resp, coro_http::status_type::ok,
              SnapshotToJson(benchmark_runner_.GetSnapshot()));
}

void HttpTestService::HandleBenchmarkStop(coro_http::coro_http_request&,
                                          coro_http::coro_http_response& resp) {
    std::string error_message;
    if (!benchmark_runner_.Stop(error_message)) {
        WriteError(resp, coro_http::status_type::conflict, error_message);
        return;
    }
    WriteJson(resp, coro_http::status_type::ok,
              SnapshotToJson(benchmark_runner_.GetSnapshot()));
}

void HttpTestService::HandleBenchmarkStatus(coro_http::coro_http_request&,
                                            coro_http::coro_http_response& resp) {
    WriteJson(resp, coro_http::status_type::ok,
              SnapshotToJson(benchmark_runner_.GetSnapshot()));
}

Json::Value HttpTestService::SnapshotToJson(const BenchmarkSnapshot& snapshot) {
    Json::Value root(Json::objectValue);
    root["state"] = [state = snapshot.state]() {
        switch (state) {
            case BenchmarkState::Idle:
                return "idle";
            case BenchmarkState::Running:
                return "running";
            case BenchmarkState::Stopping:
                return "stopping";
            case BenchmarkState::Completed:
                return "completed";
            case BenchmarkState::Failed:
                return "failed";
        }
        return "unknown";
    }();
    root["run_id"] = snapshot.run_id;

    Json::Value config(Json::objectValue);
    config["key_prefix"] = snapshot.config.key_prefix;
    config["object_size"] =
        static_cast<Json::UInt64>(snapshot.config.object_size);
    config["batch_size"] =
        static_cast<Json::UInt64>(snapshot.config.batch_size);
    config["iterations"] =
        static_cast<Json::UInt64>(snapshot.config.iterations);
    config["concurrency"] =
        static_cast<Json::UInt64>(snapshot.config.concurrency);
    config["cleanup"] = snapshot.config.cleanup;
    config["replica_num"] =
        static_cast<Json::UInt64>(snapshot.config.replica_num);
    config["seed"] = static_cast<Json::UInt64>(snapshot.config.seed);
    root["config"] = std::move(config);

    root["started_at_ms"] = static_cast<Json::UInt64>(snapshot.started_at_ms);
    root["elapsed_ms"] = static_cast<Json::UInt64>(snapshot.elapsed_ms);
    root["total_put_ops"] = static_cast<Json::UInt64>(snapshot.total_put_ops);
    root["total_get_ops"] = static_cast<Json::UInt64>(snapshot.total_get_ops);
    root["bytes_written"] = static_cast<Json::UInt64>(snapshot.bytes_written);
    root["bytes_read"] = static_cast<Json::UInt64>(snapshot.bytes_read);
    root["put_failures"] = static_cast<Json::UInt64>(snapshot.put_failures);
    root["get_failures"] = static_cast<Json::UInt64>(snapshot.get_failures);
    root["verify_failures"] =
        static_cast<Json::UInt64>(snapshot.verify_failures);
    root["cleanup_failures"] =
        static_cast<Json::UInt64>(snapshot.cleanup_failures);
    root["put_ops_per_sec"] = snapshot.put_ops_per_sec;
    root["get_ops_per_sec"] = snapshot.get_ops_per_sec;
    root["write_mib_per_sec"] = snapshot.write_mib_per_sec;
    root["read_mib_per_sec"] = snapshot.read_mib_per_sec;
    root["avg_batch_put_ms"] = snapshot.avg_batch_put_ms;
    root["avg_batch_get_ms"] = snapshot.avg_batch_get_ms;
    root["p50_batch_put_ms"] = snapshot.p50_batch_put_ms;
    root["p95_batch_put_ms"] = snapshot.p95_batch_put_ms;
    root["p50_batch_get_ms"] = snapshot.p50_batch_get_ms;
    root["p95_batch_get_ms"] = snapshot.p95_batch_get_ms;
    root["last_error"] = snapshot.last_error;
    return root;
}

Json::Value HttpTestService::ReplicaDescriptorToJson(
    const mooncake::Replica::Descriptor& descriptor) {
    Json::Value root(Json::objectValue);
    root["id"] = static_cast<Json::UInt64>(descriptor.id);
    root["status"] = [&descriptor]() {
        std::ostringstream os;
        os << descriptor.status;
        return os.str();
    }();

    if (descriptor.is_memory_replica()) {
        const auto& memory = descriptor.get_memory_descriptor();
        root["type"] = "MEMORY";
        root["object_size"] =
            static_cast<Json::UInt64>(memory.buffer_descriptor.size_);
        root["transport_endpoint"] =
            memory.buffer_descriptor.transport_endpoint_;
        root["file_path"] = Json::nullValue;
        root["buffer_address"] = static_cast<Json::UInt64>(
            memory.buffer_descriptor.buffer_address_);
        root["buffer_size"] =
            static_cast<Json::UInt64>(memory.buffer_descriptor.size_);
    } else if (descriptor.is_disk_replica()) {
        const auto& disk = descriptor.get_disk_descriptor();
        root["type"] = "DISK";
        root["object_size"] = static_cast<Json::UInt64>(disk.object_size);
        root["transport_endpoint"] = Json::nullValue;
        root["file_path"] = disk.file_path;
        root["buffer_address"] = Json::nullValue;
        root["buffer_size"] = Json::nullValue;
    } else if (descriptor.is_local_disk_replica()) {
        const auto& local_disk = descriptor.get_local_disk_descriptor();
        root["type"] = "LOCAL_DISK";
        root["object_size"] =
            static_cast<Json::UInt64>(local_disk.object_size);
        root["transport_endpoint"] = local_disk.transport_endpoint;
        root["file_path"] = Json::nullValue;
        root["buffer_address"] = Json::nullValue;
        root["buffer_size"] = Json::nullValue;
    } else {
        root["type"] = "UNKNOWN";
        root["object_size"] = Json::nullValue;
        root["transport_endpoint"] = Json::nullValue;
        root["file_path"] = Json::nullValue;
        root["buffer_address"] = Json::nullValue;
        root["buffer_size"] = Json::nullValue;
    }

    return root;
}

Json::Value HttpTestService::ReplicaDescriptorsToJson(
    const std::vector<mooncake::Replica::Descriptor>& descriptors) {
    Json::Value root(Json::arrayValue);
    for (const auto& descriptor : descriptors) {
        root.append(ReplicaDescriptorToJson(descriptor));
    }
    return root;
}

std::string HttpTestService::SerializeJson(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

void HttpTestService::WriteJson(coro_http::coro_http_response& resp,
                                coro_http::status_type status,
                                const Json::Value& body) {
    resp.add_header("Content-Type", "application/json; charset=utf-8");
    resp.set_status_and_content(status, SerializeJson(body));
}

void HttpTestService::WriteError(coro_http::coro_http_response& resp,
                                 coro_http::status_type status,
                                 const std::string& message) {
    Json::Value body(Json::objectValue);
    body["error"] = message;
    WriteJson(resp, status, body);
}

std::optional<std::string> HttpTestService::ExtractObjectKey(std::string_view url,
                                                             bool meta_endpoint) {
    constexpr std::string_view kPrefix = "/api/object/";
    constexpr std::string_view kMetaSuffix = "/meta";
    url = TrimPrefix(url, kPrefix);
    if (meta_endpoint) {
        if (url.size() <= kMetaSuffix.size() ||
            url.substr(url.size() - kMetaSuffix.size()) != kMetaSuffix) {
            return std::nullopt;
        }
        url = url.substr(0, url.size() - kMetaSuffix.size());
    }
    if (url.empty()) {
        return std::nullopt;
    }
    return UrlDecode(url);
}

std::optional<std::vector<std::string>> HttpTestService::ParseKeysFromBody(
    std::string_view body, std::string& error_message) {
    auto json = ParseJsonBody(std::string(body), error_message);
    if (!json.has_value()) {
        return std::nullopt;
    }

    const auto& keys = (*json)["keys"];
    if (!keys.isArray() || keys.empty()) {
        error_message = "keys must be a non-empty array";
        return std::nullopt;
    }

    std::vector<std::string> result;
    result.reserve(keys.size());
    for (const auto& key : keys) {
        if (!key.isString() || key.asString().empty()) {
            error_message = "keys must contain non-empty strings";
            return std::nullopt;
        }
        result.push_back(key.asString());
    }
    return result;
}

std::string HttpTestService::UrlDecode(std::string_view encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '%' && index + 2 < encoded.size() &&
            std::isxdigit(static_cast<unsigned char>(encoded[index + 1])) &&
            std::isxdigit(static_cast<unsigned char>(encoded[index + 2]))) {
            const auto hex = std::string(encoded.substr(index + 1, 2));
            decoded.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
            index += 2;
        } else if (encoded[index] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(encoded[index]);
        }
    }
    return decoded;
}

std::string HttpTestService::DeriveIpcSocketPath(
    const std::string& real_client_address,
    const std::string& configured_ipc_socket_path) {
    if (!configured_ipc_socket_path.empty()) {
        return configured_ipc_socket_path;
    }

    const auto colon_pos = real_client_address.rfind(':');
    if (colon_pos == std::string::npos || colon_pos + 1 >= real_client_address.size()) {
        throw std::invalid_argument(
            "real_client_address must be in host:port format when "
            "ipc_socket_path is not provided");
    }

    return "@mooncake_client_" + real_client_address.substr(colon_pos + 1) +
           ".sock";
}

std::optional<std::size_t> HttpTestService::ParseSizeT(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t pos = 0;
        const auto parsed = std::stoull(std::string(value), &pos, 10);
        if (pos != value.size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string HttpTestService::HealthStatusToString(int health_code) {
    switch (health_code) {
        case HC_HEALTHY:
            return "healthy";
        case HC_NOT_INITIALIZED:
            return "not_initialized";
        case HC_MASTER_UNREACHABLE:
            return "realclient_unreachable";
        default:
            return "unknown";
    }
}

}  // namespace mooncake::tools
