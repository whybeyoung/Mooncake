#include "kvconnector.h"
#include "kvconnector_wrapper.h"

#include <chrono>
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glog/logging.h>

#include "allocator.h"
#include "client_buffer.hpp"
#include "real_client.h"
#include "replica.h"
#include "utils.h"

namespace {

constexpr int kDefaultLocalRpcPort = 50052;

struct ConnectorState {
    std::shared_ptr<mooncake::RealClient> real_client;
    std::shared_ptr<mooncake::DummyClient> dummy_client;
    std::string server_address;
    std::string ipc_socket_path;
    int local_rpc_port{kDefaultLocalRpcPort};
};

std::mutex& StateMutex() {
    static std::mutex mu;
    return mu;
}

std::unordered_map<const KVConnector*, std::unique_ptr<ConnectorState>>&
StateMap() {
    static auto* states =
        new std::unordered_map<const KVConnector*, std::unique_ptr<ConnectorState>>();
    return *states;
}

ConnectorState* GetState(const KVConnector* connector) {
    std::lock_guard<std::mutex> lock(StateMutex());
    auto it = StateMap().find(connector);
    return it == StateMap().end() ? nullptr : it->second.get();
}

ConnectorState& EnsureState(const KVConnector* connector) {
    std::lock_guard<std::mutex> lock(StateMutex());
    auto& entry = StateMap()[connector];
    if (!entry) {
        entry = std::make_unique<ConnectorState>();
    }
    return *entry;
}

void EraseState(const KVConnector* connector) {
    std::lock_guard<std::mutex> lock(StateMutex());
    StateMap().erase(connector);
}

std::string ExtractHost(std::string_view host_or_host_port) {
    const std::size_t colon = host_or_host_port.rfind(':');
    if (colon == std::string_view::npos) {
        return std::string(host_or_host_port);
    }
    return std::string(host_or_host_port.substr(0, colon));
}

int AcquireFreePort() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return kDefaultLocalRpcPort;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(sock);
        return kDefaultLocalRpcPort;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(sock);
        return kDefaultLocalRpcPort;
    }
    const int port = ntohs(addr.sin_port);
    ::close(sock);
    return port > 0 ? port : kDefaultLocalRpcPort;
}

std::string BuildIpcSocketPath(const KVConnector* connector) {
    static std::atomic<uint64_t> counter{0};
    return "mooncake_connector_" + std::to_string(::getpid()) + "_" +
           std::to_string(reinterpret_cast<uintptr_t>(connector)) + "_" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

mooncake::ReplicateConfig BuildReplicateConfig(
    size_t replica_num, bool with_soft_pin,
    const std::string& preferred_segment) {
    mooncake::ReplicateConfig config;
    config.replica_num = replica_num == 0 ? 1 : replica_num;
    config.with_soft_pin = with_soft_pin;
    config.preferred_segment = preferred_segment;
    if (!preferred_segment.empty()) {
        config.preferred_segments.assign(config.replica_num, preferred_segment);
    }
    return config;
}

void TearDownClients(ConnectorState* state) {
    if (state == nullptr) {
        return;
    }
    if (state->dummy_client) {
        state->dummy_client->tearDownAll();
        state->dummy_client.reset();
    }
    if (state->real_client) {
        state->real_client->tearDownAll();
        state->real_client.reset();
    }
}

}  // namespace

bytes::bytes()
    : str(new char[1], [](char* p) { delete[] p; }), length(0) {
    str.get()[0] = '\0';
}

bytes::bytes(char* raw, uint64_t str_len) : length(str_len) {
    if (raw == nullptr || str_len == 0) {
        str = std::shared_ptr<char>(new char[1], [](char* p) { delete[] p; });
        str.get()[0] = '\0';
        length = 0;
        return;
    }
    str = std::shared_ptr<char>(raw, [](char* p) { delete[] p; });
}

const char* bytes::data() const { return str.get(); }

uint64_t bytes::size() const { return length; }

KVConnector::KVConnector()
    : thread_pool_manager_(std::make_unique<ThreadPoolManager>()) {
    segment_ptr_ = nullptr;
    base_memory_ptr_ = nullptr;
    global_segment_size_ = 0;
    (void)EnsureState(this);
}

KVConnector::~KVConnector() {
    if (client_ && segment_ptr_ && global_segment_size_) {
        auto unmount_result =
            client_->UnmountSegment(segment_ptr_, global_segment_size_);
        if (!unmount_result.has_value()) {
            LOG(ERROR) << "Failed to unmount segment in destructor: "
                       << toString(unmount_result.error());
        }
    }
    if (segment_ptr_ != nullptr) {
        free(segment_ptr_);
        segment_ptr_ = nullptr;
    }
    if (base_memory_ptr_ != nullptr) {
        free(base_memory_ptr_);
        base_memory_ptr_ = nullptr;
    }
    client_buffer_allocator_.reset();
    if (auto* state = GetState(this); state != nullptr) {
        TearDownClients(state);
    }
    EraseState(this);
}

int KVConnector::setup(const std::string& local_hostname,
                       const std::string& metadata_server,
                       const std::string& protocol,
                       const std::string& rdma_devices,
                       const std::string& master_server_addr,
                       size_t global_segment_size, size_t local_buffer_size,
                       const std::string& memory_allocator,
                       wrapperMetrics) {
    this->protocol = protocol;
    this->local_hostname = local_hostname;
    this->global_segment_size_ = global_segment_size;

    if (local_buffer_size == 0) {
        LOG(ERROR) << "local_buffer_size invalid";
        return -6;
    }

    auto& state = EnsureState(this);
    TearDownClients(&state);
    if (base_memory_ptr_ != nullptr) {
        free(base_memory_ptr_);
        base_memory_ptr_ = nullptr;
    }
    if (segment_ptr_ != nullptr) {
        free(segment_ptr_);
        segment_ptr_ = nullptr;
    }
    client_buffer_allocator_.reset();
    client_.reset();

    state.real_client = mooncake::RealClient::create();
    state.dummy_client = std::make_shared<mooncake::DummyClient>();
    state.ipc_socket_path = BuildIpcSocketPath(this);
    state.local_rpc_port = AcquireFreePort();
    state.server_address =
        ExtractHost(local_hostname) + ":" + std::to_string(state.local_rpc_port);

    auto setup_result = state.real_client->setup_internal(
        local_hostname, metadata_server, global_segment_size, local_buffer_size,
        protocol, rdma_devices, master_server_addr, nullptr,
        state.ipc_socket_path, state.local_rpc_port, false);
    if (!setup_result.has_value()) {
        LOG(ERROR) << "RealClient setup failed: "
                   << mooncake::toString(setup_result.error());
        TearDownClients(&state);
        return -1;
    }

    base_memory_ptr_ =
        allocate_buffer_allocator_memory(local_buffer_size, protocol);
    if (base_memory_ptr_ == nullptr) {
        LOG(ERROR) << "Failed to allocate base memory for buffer allocator";
        TearDownClients(&state);
        return -2;
    }

    const size_t base_addr = reinterpret_cast<size_t>(base_memory_ptr_);
    if (memory_allocator == "cachelib") {
        client_buffer_allocator_ =
            std::make_shared<mooncake::CachelibBufferAllocator>(
                "kvcache_cachelib_buffer", base_addr, local_buffer_size, "");
    } else {
        client_buffer_allocator_ =
            std::make_shared<mooncake::OffsetBufferAllocator>(
                "kvcache_offset_buffer", base_addr, local_buffer_size, "");
    }

    if (!client_buffer_allocator_) {
        LOG(ERROR) << "Failed to create buffer allocator";
        free(base_memory_ptr_);
        base_memory_ptr_ = nullptr;
        TearDownClients(&state);
        return -2;
    }

    const std::size_t mem_pool_size =
        std::max(global_segment_size, local_buffer_size);
    const int dummy_rc = state.dummy_client->setup_dummy(
        mem_pool_size, local_buffer_size, state.server_address,
        state.ipc_socket_path);
    if (dummy_rc != 0) {
        LOG(ERROR) << "DummyClient setup failed, server_address="
                   << state.server_address;
        client_buffer_allocator_.reset();
        free(base_memory_ptr_);
        base_memory_ptr_ = nullptr;
        TearDownClients(&state);
        return -1;
    }

    this->client_ = state.real_client->client_;
    this->device_name = rdma_devices;
    return 0;
}

int KVConnector::initAll(const std::string& local_hostname,
                         const std::string& metadata_server,
                         const std::string& protocol,
                         const std::string& device_name,
                         const std::string& master_server_addr,
                         size_t mount_segment_size,
                         size_t buffer_allocator_size,
                         const std::string& memory_allocator,
                         wrapperMetrics cb) {
    return setup(local_hostname, metadata_server, protocol, device_name,
                 master_server_addr, mount_segment_size, buffer_allocator_size,
                 memory_allocator, cb);
}

int KVConnector::put(const std::string& key, const std::string& value,
                     size_t replica_num,
                     const std::string& preferred_segment) {
    auto* state = GetState(this);
    if (state == nullptr || !state->dummy_client) {
        return 1;
    }
    if (key.empty()) {
        return 1;
    }
    const auto config =
        BuildReplicateConfig(replica_num, false, preferred_segment);
    const int rc = state->dummy_client->put(
        key, std::span<const char>(value.data(), value.size()), config);
    return rc == 0 ? 0 : 1;
}

bytes KVConnector::get(const std::string& key) {
    auto* state = GetState(this);
    if (state == nullptr || !state->dummy_client || key.empty()) {
        return bytes();
    }

    auto replica_descriptors = state->dummy_client->get_replica_desc(key);
    if (replica_descriptors.empty()) {
        return bytes();
    }

    auto handle = state->dummy_client->get_buffer(key);
    if (!handle || handle->ptr() == nullptr || handle->size() == 0) {
        return bytes();
    }

    std::vector<mooncake::Slice> slices{{handle->ptr(), handle->size()}};
    char* str = exportSlices(slices, handle->size());
    if (str == nullptr) {
        return bytes();
    }
    return bytes(str, handle->size());
}

int KVConnector::remove(const std::string& key) {
    auto* state = GetState(this);
    if (state == nullptr || !state->dummy_client) {
        return -1;
    }
    if (key.empty()) {
        return -1;
    }
    auto remove_result = state->dummy_client->remove(key);
    if (remove_result != 0) {
        LOG(ERROR) << "delete key err[key=" << key
                   << ", error_code=" << remove_result << "]";
        return -1;
    }
    return 0;
}

int KVConnector::exist(const std::string& key) {
    auto* state = GetState(this);
    if (state == nullptr || !state->dummy_client) {
        return -1;
    }
    if (key.empty()) {
        return -1;
    }

    const int rc = state->dummy_client->isExist(key);
    if (rc == 1) {
        return 0;
    }
    if (rc == 0) {
        return 1;
    }
    return -1;
}

std::vector<int> KVConnector::batchExist(const std::vector<std::string>& keys) {
    auto* state = GetState(this);
    if (state == nullptr || !state->dummy_client) {
        return std::vector<int>(keys.size(), -1);
    }

    auto results = state->dummy_client->batchIsExist(keys);
    for (auto& rc : results) {
        if (rc == 1) {
            rc = 0;
        } else if (rc == 0) {
            rc = 1;
        } else {
            rc = -1;
        }
    }
    return results;
}

int KVConnector::batchPut(const std::vector<std::string>& keys,
                          const std::vector<std::string>& values,
                          size_t replica_num, bool with_soft_pin,
                          const std::string& preferred_segment) {
    auto* state = GetState(this);
    if (state == nullptr || !state->dummy_client) {
        return -4;
    }
    if (keys.size() != values.size() || keys.empty()) {
        return keys.empty() ? -2 : -1;
    }

    std::vector<std::span<const char>> spans;
    spans.reserve(values.size());
    for (const auto& value : values) {
        spans.emplace_back(value.data(), value.size());
    }

    const auto config =
        BuildReplicateConfig(replica_num, with_soft_pin, preferred_segment);
    const int rc = state->dummy_client->put_batch(keys, spans, config);
    return rc == 0 ? 0 : -4;
}

int KVConnector::batchPut(const std::vector<std::string>& keys,
                          const std::vector<mooncake::Slice>& values,
                          size_t replica_num, bool with_soft_pin,
                          const std::string& preferred_segment) {
    auto* state = GetState(this);
    if (state == nullptr || !state->dummy_client) {
        return -4;
    }
    if (keys.size() != values.size() || keys.empty()) {
        return keys.empty() ? -2 : -1;
    }

    std::vector<std::span<const char>> spans;
    spans.reserve(values.size());
    for (const auto& value : values) {
        if (value.ptr == nullptr) {
            return -3;
        }
        spans.emplace_back(static_cast<const char*>(value.ptr), value.size);
    }

    const auto config =
        BuildReplicateConfig(replica_num, with_soft_pin, preferred_segment);
    const int rc = state->dummy_client->put_batch(keys, spans, config);
    return rc == 0 ? 0 : -4;
}

int KVConnector::batchGet(const std::vector<std::string>& keys,
                          std::unordered_map<std::string, mooncake::Slice>& result) {
    auto* state = GetState(this);
    if (state == nullptr || !state->dummy_client) {
        return -5;
    }
    result.clear();
    if (keys.empty()) {
        LOG(ERROR) << "batchGet: keys is empty";
        return -1;
    }

    auto replica_descs = state->dummy_client->batch_get_replica_desc(keys);
    if (replica_descs.size() != keys.size()) {
        LOG(ERROR) << "batchGet: batch_query_results exist empty item";
        return -2;
    }

    auto handles = state->dummy_client->batch_get_buffer(keys);
    if (handles.size() != keys.size()) {
        LOG(ERROR) << "batchGet: batch_get_results size mismatch";
        return -4;
    }

    std::unordered_map<std::string, mooncake::Slice> temp_results;
    temp_results.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& handle = handles[i];
        if (!handle || handle->ptr() == nullptr) {
            for (auto& [_, slice] : temp_results) {
                delete[] static_cast<char*>(slice.ptr);
            }
            LOG(ERROR) << "batchGet: batch_get_results exist empty item";
            return -5;
        }

        std::vector<mooncake::Slice> slices{{handle->ptr(), handle->size()}};
        char* data = exportSlices(slices, handle->size());
        if (data == nullptr) {
            for (auto& [_, slice] : temp_results) {
                delete[] static_cast<char*>(slice.ptr);
            }
            LOG(ERROR) << "batchGet: handle final data err";
            return -7;
        }
        temp_results.emplace(keys[i], mooncake::Slice{data, handle->size()});
    }

    result = std::move(temp_results);
    return 0;
}

int KVConnector::allocateSlices(
    std::vector<mooncake::Slice>& slices, const std::string& value,
    std::vector<std::unique_ptr<mooncake::AllocatedBuffer>>& buffers) {
    return allocateSlices(slices, value.data(), value.size(), buffers);
}

int KVConnector::allocateSlices(
    std::vector<mooncake::Slice>& slices,
    const std::vector<mooncake::Replica::Descriptor>& replica_descriptors,
    uint64_t& length,
    std::vector<std::unique_ptr<mooncake::AllocatedBuffer>>& buffers) {
    slices.clear();
    buffers.clear();
    length = 0;
    if (replica_descriptors.empty() || !client_buffer_allocator_) {
        return -1;
    }

    const auto& replica_desc = replica_descriptors.front();
    length = mooncake::calculate_total_size(replica_desc);
    auto allocated_buffer = client_buffer_allocator_->allocate(length);
    if (!allocated_buffer) {
        return 1;
    }
    void* ptr = allocated_buffer->data();
    buffers.push_back(std::move(allocated_buffer));
    return mooncake::allocateSlices(slices, replica_desc, ptr);
}

int KVConnector::allocateSlices(
    std::vector<mooncake::Slice>& slices, const char* data_ptr, size_t data_len,
    std::vector<std::unique_ptr<mooncake::AllocatedBuffer>>& buffers) {
    slices.clear();
    buffers.clear();
    if (data_ptr == nullptr && data_len != 0) {
        return 1;
    }
    if (!client_buffer_allocator_) {
        return 1;
    }

    uint64_t offset = 0;
    while (offset < data_len) {
        auto chunk_size = std::min<uint64_t>(data_len - offset, kMaxSliceSize);
        auto allocated_buffer = client_buffer_allocator_->allocate(chunk_size);
        if (!allocated_buffer) {
            slices.clear();
            buffers.clear();
            return 1;
        }
        void* ptr = allocated_buffer->data();
        std::memcpy(ptr, data_ptr + offset, chunk_size);
        buffers.push_back(std::move(allocated_buffer));
        slices.emplace_back(mooncake::Slice{ptr, chunk_size});
        offset += chunk_size;
    }
    return 0;
}

char* KVConnector::exportSlices(const std::vector<mooncake::Slice>& slices,
                                uint64_t length) {
    if (length == 0) {
        LOG(ERROR) << "exportSlices: invalid length (0)";
        return nullptr;
    }
    auto* data = new char[length + 1];
    data[length] = '\0';
    uint64_t offset = 0;
    for (const auto& slice : slices) {
        if (slice.ptr == nullptr || slice.size == 0) {
            LOG(ERROR)
                << "exportSlices: invalid slice (ptr=nullptr or size=0)";
            delete[] data;
            return nullptr;
        }
        if (offset + slice.size > length) {
            LOG(ERROR) << "exportSlices: slice overflow (offset=" << offset
                       << ", size=" << slice.size << ", total=" << length
                       << ")";
            delete[] data;
            return nullptr;
        }
        std::memcpy(data + offset, slice.ptr, slice.size);
        offset += slice.size;
    }
    if (offset != length) {
        LOG(WARNING) << "exportSlices: size mismatch (expected=" << length
                     << ", actual=" << offset << ")";
    }
    return data;
}

int KVConnector::freeSlices(
    std::vector<mooncake::Slice>& slices,
    std::vector<std::unique_ptr<mooncake::AllocatedBuffer>>& buffers) {
    slices.clear();
    buffers.clear();
    return 0;
}
