#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <span>
#include <vector>

#include "dummy_client.h"

namespace mooncake::tools {

/// @brief KVStore client wrapper for performance testing.
/// @note This class provides a simple interface for put/get/exist operations.
class KvStoreClient {
   public:
    /// @brief Constructs a new KvStoreClient.
    KvStoreClient();

    /// @brief Destroys the KvStoreClient.
    ~KvStoreClient();

    KvStoreClient(const KvStoreClient&) = delete;
    KvStoreClient& operator=(const KvStoreClient&) = delete;
    KvStoreClient(KvStoreClient&&) = delete;
    KvStoreClient& operator=(KvStoreClient&&) = delete;

    /// @brief Sets up the client connection.
    /// @param mem_pool_size Memory pool size in bytes.
    /// @param local_buffer_size Local buffer size in bytes.
    /// @param real_client_address Real client RPC address.
    /// @param ipc_socket_path IPC socket path.
    /// @return 0 on success, non-zero on failure.
    int Setup(std::size_t mem_pool_size, std::size_t local_buffer_size,
              const std::string& real_client_address,
              const std::string& ipc_socket_path);

    /// @brief Stores a value with the specified key.
    /// @param key The key to store.
    /// @param value The value to store.
    /// @param replica_num Number of replicas (default: 1).
    /// @return 0 on success, non-zero on failure.
    int Put(const std::string& key, const std::string& value,
            std::size_t replica_num = 1);

    /// @brief Stores a value with the specified key (zero-copy input).
    /// @param key The key to store.
    /// @param value Read-only value bytes.
    /// @param replica_num Number of replicas (default: 1).
    /// @return 0 on success, non-zero on failure.
    int Put(const std::string& key, std::span<const char> value,
            std::size_t replica_num = 1);

    /// @brief Retrieves a value by key.
    /// @param key The key to retrieve.
    /// @param value Output parameter for the retrieved value.
    /// @return 0 on success, non-zero on failure.
    int Get(const std::string& key, std::string& value);

    /// @brief Checks if a key exists.
    /// @param key The key to check.
    /// @return 1 if exists, 0 if not exists, -1 on error.
    int Exist(const std::string& key);

    /// @brief Stores multiple key-value pairs.
    /// @param keys The keys to store.
    /// @param values The values to store.
    /// @param replica_num Number of replicas (default: 1).
    /// @return Vector of result codes for each put operation.
    std::vector<int> BatchPut(const std::vector<std::string>& keys,
                              const std::vector<std::string>& values,
                              std::size_t replica_num = 1);

    /// @brief Stores multiple keys with the same value (zero-copy input).
    /// @param keys The keys to store.
    /// @param value Read-only value bytes for all keys.
    /// @param replica_num Number of replicas (default: 1).
    /// @return Vector of result codes for each put operation.
    std::vector<int> BatchPut(const std::vector<std::string>& keys,
                                std::span<const char> value,
                                std::size_t replica_num = 1);

    /// @brief Retrieves multiple values by keys.
    /// @param keys The keys to retrieve.
    /// @param values Output vector for the retrieved values.
    /// @return Vector of results: 0 on success, -1 on failure.
    std::vector<int> BatchGet(const std::vector<std::string>& keys,
                              std::vector<std::string>& values);

    /// @brief Checks if multiple keys exist.
    /// @param keys The keys to check.
    /// @return Vector of results: 1 if exists, 0 if not exists, -1 on error.
    std::vector<int> BatchExist(const std::vector<std::string>& keys);

    /// @brief Removes a key.
    /// @param key The key to remove.
    /// @return 0 on success, non-zero on failure.
    int Remove(const std::string& key);

    /// @brief Removes all keys matching the regex pattern.
    /// @param regex The regex pattern.
    /// @param force Force removal.
    /// @return Number of removed keys, or -1 on error.
    long RemoveByRegex(const std::string& regex, bool force);

    /// @brief Removes all keys.
    /// @param force Force removal.
    /// @return Number of removed keys, or -1 on error.
    long RemoveAll(bool force);

    /// @brief Performs a health check.
    /// @return Health status code.
    int HealthCheck();

   private:
    std::shared_ptr<mooncake::DummyClient> client_;
};

}  // namespace mooncake::tools
