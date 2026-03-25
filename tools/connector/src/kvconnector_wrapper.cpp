#include "kvconnector_wrapper.h"

#include "kvconnector.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glog/logging.h>

namespace {

std::mutex& HandlerMutex() {
    static std::mutex mu;
    return mu;
}

std::unique_ptr<KVConnector>& GlobalConnector() {
    static auto* connector = new std::unique_ptr<KVConnector>();
    return *connector;
}

std::string ParamValue(const CacheConfig* config, const char* key) {
    if (config == nullptr || config->desc == nullptr || key == nullptr) {
        return "";
    }
    for (auto* cur = config->desc; cur != nullptr; cur = cur->next) {
        if (cur->key != nullptr && std::strcmp(cur->key, key) == 0 &&
            cur->value != nullptr) {
            return std::string(cur->value, cur->vlen);
        }
    }
    return "";
}

size_t ParamSize(const CacheConfig* config, const char* key,
                 size_t default_value) {
    const std::string value = ParamValue(config, key);
    if (value.empty()) {
        return default_value;
    }
    return static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
}

bool ParamBool(const CacheConfig* config, const char* key, bool default_value) {
    const std::string value = ParamValue(config, key);
    if (value.empty()) {
        return default_value;
    }
    return value == "1" || value == "true" || value == "TRUE";
}

char* DupCString(const char* src, size_t len) {
    auto* out = static_cast<char*>(std::malloc(len + 1));
    if (out == nullptr) {
        return nullptr;
    }
    if (len != 0 && src != nullptr) {
        std::memcpy(out, src, len);
    }
    out[len] = '\0';
    return out;
}

DataList* MakeDataNode(const std::string& key, const void* data, size_t len) {
    auto* node = static_cast<DataList*>(std::calloc(1, sizeof(DataList)));
    if (node == nullptr) {
        return nullptr;
    }
    node->key = DupCString(key.data(), key.size());
    node->data = std::malloc(len == 0 ? 1 : len);
    if (node->key == nullptr || node->data == nullptr) {
        std::free(node->key);
        std::free(node->data);
        std::free(node);
        return nullptr;
    }
    if (len != 0 && data != nullptr) {
        std::memcpy(node->data, data, len);
    }
    node->len = static_cast<unsigned int>(len);
    node->type = DataRaw;
    node->status = DataOnce;
    return node;
}

int FreeDataListImpl(pDataList data) {
    while (data != nullptr) {
        auto* next = data->next;
        std::free(data->key);
        std::free(data->data);
        std::free(data);
        data = next;
    }
    return KVCONNECTOR_SUCCESS;
}

std::vector<std::string> CollectKeys(const KeyList* keys) {
    std::vector<std::string> out;
    for (auto* cur = keys; cur != nullptr; cur = cur->next) {
        if (cur->key != nullptr && cur->key[0] != '\0') {
            out.emplace_back(cur->key);
        }
    }
    return out;
}

int ToDeleteResult(int total, int success) {
    if (success == total) {
        return KVCONNECTOR_SUCCESS;
    }
    if (success > 0) {
        return KVCONNECTOR_ERR_PARTIAL_DEL;
    }
    return KVCONNECTOR_ERR_BACKEND_OP;
}

int ToExistResult(int total, int success, int backend_failures) {
    if (backend_failures != 0) {
        return KVCONNECTOR_ERR_BACKEND_OP;
    }
    if (success == total) {
        return KVCONNECTOR_SUCCESS;
    }
    if (success == 0) {
        return KVCONNECTOR_ERR_NOT_EXIST;
    }
    return KVCONNECTOR_ERR_PARTIAL_EXIST;
}

int HandlerPut(const KeyList* keys, pDataList inData, const CacheConfig* config) {
    std::lock_guard<std::mutex> lock(HandlerMutex());
    if (!GlobalConnector()) {
        return KVCONNECTOR_ERR_CONNECTOR_DESTROYED;
    }

    std::vector<std::string> key_vec;
    std::vector<mooncake::Slice> value_vec;
    auto* k = keys;
    auto* d = inData;
    for (; k != nullptr && d != nullptr; k = k->next, d = d->next) {
        if (k->key == nullptr || k->key[0] == '\0') {
            return KVCONNECTOR_ERR_KEY_INVALID;
        }
        if (d->data == nullptr && d->len != 0) {
            return KVCONNECTOR_ERR_DATA_INVALID;
        }
        key_vec.emplace_back(k->key);
        value_vec.push_back(mooncake::Slice{d->data, d->len});
    }

    if (key_vec.empty()) {
        return KVCONNECTOR_ERR_INVALID_PARAM;
    }

    const size_t replica_num = ParamSize(config, "replica_num", 1);
    const bool with_soft_pin = ParamBool(config, "with_soft_pin", false);
    const std::string preferred_segment =
        ParamValue(config, "preferred_segment");
    const int rc = GlobalConnector()->batchPut(key_vec, value_vec, replica_num,
                                               with_soft_pin,
                                               preferred_segment);
    if (rc == 0) {
        return KVCONNECTOR_SUCCESS;
    }
    if (rc == -1 || rc == -2 || rc == -3) {
        return KVCONNECTOR_ERR_INVALID_PARAM;
    }
    return KVCONNECTOR_ERR_BACKEND_OP;
}

int HandlerGet(const KeyList* keys, pDataList* outData, const CacheConfig*) {
    std::lock_guard<std::mutex> lock(HandlerMutex());
    if (outData == nullptr) {
        return KVCONNECTOR_ERR_INVALID_PARAM;
    }
    *outData = nullptr;
    if (!GlobalConnector()) {
        return KVCONNECTOR_ERR_CONNECTOR_DESTROYED;
    }

    const auto key_vec = CollectKeys(keys);
    if (key_vec.empty()) {
        return KVCONNECTOR_ERR_INVALID_PARAM;
    }

    std::unordered_map<std::string, mooncake::Slice> results;
    const int rc = GlobalConnector()->batchGet(key_vec, results);
    if (rc != 0 && results.empty()) {
        return rc == -1 ? KVCONNECTOR_ERR_INVALID_PARAM
                        : KVCONNECTOR_ERR_DATA_NOT_FOUND;
    }

    DataList* head = nullptr;
    DataList* tail = nullptr;
    for (const auto& key : key_vec) {
        auto it = results.find(key);
        if (it == results.end()) {
            continue;
        }
        DataList* node = MakeDataNode(key, it->second.ptr, it->second.size);
        delete[] static_cast<char*>(it->second.ptr);
        if (node == nullptr) {
            FreeDataListImpl(head);
            return KVCONNECTOR_ERR_MEMORY_ALLOC;
        }
        if (head == nullptr) {
            head = node;
        } else {
            tail->next = node;
        }
        tail = node;
    }

    *outData = head;
    if (head == nullptr) {
        return KVCONNECTOR_ERR_DATA_NOT_FOUND;
    }
    return rc == 0 ? KVCONNECTOR_SUCCESS : KVCONNECTOR_ERR_BACKEND_OP;
}

int HandlerGetToMemory(const KeyList* keys, pDataList ioData,
                       const CacheConfig*) {
    std::lock_guard<std::mutex> lock(HandlerMutex());
    if (!GlobalConnector()) {
        return KVCONNECTOR_ERR_CONNECTOR_DESTROYED;
    }

    int success = 0;
    int failed = 0;
    auto* k = keys;
    auto* d = ioData;
    for (; k != nullptr && d != nullptr; k = k->next, d = d->next) {
        if (k->key == nullptr || d->data == nullptr) {
            ++failed;
            continue;
        }
        bytes value = GlobalConnector()->get(k->key);
        if (value.size() == 0 || d->len < value.size()) {
            ++failed;
            continue;
        }
        std::memcpy(d->data, value.data(), value.size());
        d->len = static_cast<unsigned int>(value.size());
        ++success;
    }

    if (success == 0) {
        return failed == 0 ? KVCONNECTOR_ERR_DATA_NOT_FOUND
                           : KVCONNECTOR_ERR_INVALID_PARAM;
    }
    return failed == 0 ? KVCONNECTOR_SUCCESS : KVCONNECTOR_ERR_BACKEND_OP;
}

int HandlerDel(const KeyList* keys, const CacheConfig*) {
    std::lock_guard<std::mutex> lock(HandlerMutex());
    if (!GlobalConnector()) {
        return KVCONNECTOR_ERR_CONNECTOR_DESTROYED;
    }
    int total = 0;
    int success = 0;
    for (auto* cur = keys; cur != nullptr; cur = cur->next) {
        if (cur->key == nullptr || cur->key[0] == '\0') {
            continue;
        }
        ++total;
        if (GlobalConnector()->remove(cur->key) == KVCONNECTOR_SUCCESS) {
            ++success;
        }
    }
    return total == 0 ? KVCONNECTOR_ERR_INVALID_PARAM
                      : ToDeleteResult(total, success);
}

int HandlerExist(pKeyList keys, const CacheConfig*) {
    std::lock_guard<std::mutex> lock(HandlerMutex());
    if (!GlobalConnector()) {
        return KVCONNECTOR_ERR_CONNECTOR_DESTROYED;
    }
    int total = 0;
    int success = 0;
    int backend_failures = 0;
    for (auto* cur = keys; cur != nullptr; cur = cur->next) {
        if (cur->key == nullptr || cur->key[0] == '\0') {
            cur->is_exist = false;
            continue;
        }
        ++total;
        const int rc = GlobalConnector()->exist(cur->key);
        cur->is_exist = (rc == 0);
        if (rc == 0) {
            ++success;
        } else if (rc != 1) {
            ++backend_failures;
        }
    }
    return total == 0 ? KVCONNECTOR_ERR_INVALID_PARAM
                      : ToExistResult(total, success, backend_failures);
}

}  // namespace

WrapperAPI int init_mooncake_handler(CacheHandler* handler,
                                     const MoonCakeClientConfig* config) {
    VLOG(1) << "init_mooncake_handler called";
    if (handler == nullptr || config == nullptr) {
        LOG(ERROR) << "init_mooncake_handler: handler or config is null";
        return KVCONNECTOR_ERR_INIT_INVALID_PARAM;
    }
    if (config->local_hostname == nullptr || config->metadata_server == nullptr ||
        config->protocol == nullptr || config->master_server_addr == nullptr ||
        config->device_name == nullptr || config->memory_allocator == nullptr) {
        LOG(ERROR) << "init_mooncake_handler: required config field is null";
        return KVCONNECTOR_ERR_INIT_INVALID_PARAM;
    }

    VLOG(1) << "init_mooncake_handler: local_hostname=" << config->local_hostname
            << ", protocol=" << config->protocol
            << ", global_segment_size=" << config->global_segment_size
            << ", local_buffer_size=" << config->local_buffer_size;

    std::lock_guard<std::mutex> lock(HandlerMutex());
    if (!GlobalConnector()) {
        GlobalConnector() = std::make_unique<KVConnector>();
    }

    const int rc = GlobalConnector()->setup(
        config->local_hostname, config->metadata_server, config->protocol,
        config->device_name, config->master_server_addr,
        config->global_segment_size, config->local_buffer_size,
        config->memory_allocator, config->metrics_callback);
    if (rc != 0) {
        LOG(ERROR) << "init_mooncake_handler: KVConnector::setup failed, rc=" << rc;
        GlobalConnector().reset();
        switch (rc) {
            case -1:
                return KVCONNECTOR_ERR_INIT_CREATE_CLIENT;
            case -2:
                return KVCONNECTOR_ERR_INIT_ALLOC_LOCAL;
            case -3:
                return KVCONNECTOR_ERR_INIT_REGIST_LOCAL;
            case -4:
                return KVCONNECTOR_ERR_INIT_ALLOC_GLOBAL;
            case -5:
                return KVCONNECTOR_ERR_INIT_MOUNT_GLOBAL;
            case -6:
                return KVCONNECTOR_ERR_INIT_LOCAL_PARAM_INVALID;
            default:
                return KVCONNECTOR_ERR_INIT_FAILED;
        }
    }

    handler->put = &HandlerPut;
    handler->get = &HandlerGet;
    handler->freeDataList = &FreeDataListImpl;
    handler->getToMemory = &HandlerGetToMemory;
    handler->del = &HandlerDel;
    handler->exist = &HandlerExist;
    VLOG(1) << "init_mooncake_handler completed successfully";
    return KVCONNECTOR_SUCCESS;
}

WrapperAPI int destroy_mooncake_handler() {
    VLOG(1) << "destroy_mooncake_handler called";
    std::lock_guard<std::mutex> lock(HandlerMutex());
    GlobalConnector().reset();
    VLOG(1) << "destroy_mooncake_handler completed";
    return KVCONNECTOR_SUCCESS;
}
