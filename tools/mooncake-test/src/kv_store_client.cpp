#include "kv_store_client.hpp"

#include <span>

#include <glog/logging.h>

#include "pyclient.h"

namespace mooncake::tools {

KvStoreClient::KvStoreClient() = default;

KvStoreClient::~KvStoreClient() = default;

int KvStoreClient::Setup(std::size_t mem_pool_size, std::size_t local_buffer_size,
                         const std::string& real_client_address,
                         const std::string& ipc_socket_path) {
    client_ = std::make_shared<mooncake::DummyClient>();
    const int rc = client_->setup_dummy(mem_pool_size, local_buffer_size,
                                        real_client_address, ipc_socket_path);
    if (rc != 0) {
        LOG(ERROR) << "Failed to setup DummyClient: rc=" << rc;
        client_.reset();
        return rc;
    }
    LOG(INFO) << "KvStoreClient setup succeeded, real_client_address="
              << real_client_address << ", ipc_socket_path=" << ipc_socket_path;
    return 0;
}

int KvStoreClient::Put(const std::string& key, const std::string& value,
                       std::size_t replica_num) {
    if (!client_) {
        LOG(ERROR) << "Client not initialized";
        return -1;
    }

    mooncake::ReplicateConfig config;
    config.replica_num = replica_num;

    const int rc = client_->put(key, std::span<const char>(value.data(), value.size()), config);
    if (rc != 0) {
        LOG(ERROR) << "Put failed, key=" << key << ", rc=" << rc;
    }
    return rc;
}

int KvStoreClient::Put(const std::string& key, std::span<const char> value,
                       std::size_t replica_num) {
    if (!client_) {
        LOG(ERROR) << "Client not initialized";
        return -1;
    }

    mooncake::ReplicateConfig config;
    config.replica_num = replica_num;

    const int rc = client_->put(key, value, config);
    if (rc != 0) {
        LOG(ERROR) << "Put failed, key=" << key << ", rc=" << rc;
    }
    return rc;
}

int KvStoreClient::Get(const std::string& key, std::string& value) {
    if (!client_) {
        LOG(ERROR) << "Client not initialized";
        return -1;
    }

    auto handle = client_->get_buffer(key);
    if (!handle) {
        VLOG(1) << "Get failed, key=" << key << ", not found";
        return -1;
    }

    value.assign(static_cast<const char*>(handle->ptr()), handle->size());
    handle.reset();
    return 0;
}

int KvStoreClient::Exist(const std::string& key) {
    if (!client_) {
        LOG(ERROR) << "Client not initialized";
        return -1;
    }

    return client_->isExist(key);
}

std::vector<int> KvStoreClient::BatchPut(const std::vector<std::string>& keys,
                                         const std::vector<std::string>& values,
                                         std::size_t replica_num) {
    std::vector<int> results;
    results.reserve(keys.size());

    mooncake::ReplicateConfig config;
    config.replica_num = replica_num;

    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i >= values.size()) {
            results.push_back(-1);
            continue;
        }

        const int rc = client_->put(keys[i],
                                    std::span<const char>(values[i].data(), values[i].size()),
                                    config);
        results.push_back(rc);
    }

    return results;
}

std::vector<int> KvStoreClient::BatchPut(const std::vector<std::string>& keys,
                                          std::span<const char> value,
                                          std::size_t replica_num) {
    std::vector<int> results;
    results.reserve(keys.size());

    mooncake::ReplicateConfig config;
    config.replica_num = replica_num;

    for (std::size_t i = 0; i < keys.size(); ++i) {
        const int rc = client_->put(keys[i], value, config);
        results.push_back(rc);
    }

    return results;
}

std::vector<int> KvStoreClient::BatchGet(const std::vector<std::string>& keys,
                                         std::vector<std::string>& values) {
    std::vector<int> results;
    results.reserve(keys.size());
    values.clear();
    values.reserve(keys.size());

    for (const auto& key : keys) {
        auto handle = client_->get_buffer(key);
        if (!handle) {
            results.push_back(-1);
            values.emplace_back();
            continue;
        }

        values.emplace_back(static_cast<const char*>(handle->ptr()), handle->size());
        handle.reset();
        results.push_back(0);
    }

    return results;
}

std::vector<int> KvStoreClient::BatchExist(const std::vector<std::string>& keys) {
    std::vector<int> results;
    results.reserve(keys.size());

    for (const auto& key : keys) {
        results.push_back(client_->isExist(key));
    }

    return results;
}

int KvStoreClient::Remove(const std::string& key) {
    if (!client_) {
        LOG(ERROR) << "Client not initialized";
        return -1;
    }

    return client_->remove(key);
}

long KvStoreClient::RemoveByRegex(const std::string& regex, bool force) {
    if (!client_) {
        LOG(ERROR) << "Client not initialized";
        return -1;
    }

    return client_->removeByRegex(regex, force);
}

long KvStoreClient::RemoveAll(bool force) {
    if (!client_) {
        LOG(ERROR) << "Client not initialized";
        return -1;
    }

    return client_->removeAll(force);
}

int KvStoreClient::HealthCheck() {
    if (!client_) {
        return mooncake::HC_NOT_INITIALIZED;
    }
    return client_->health_check();
}

}  // namespace mooncake::tools
