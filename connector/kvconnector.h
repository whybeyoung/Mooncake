#pragma once

#include "thread_pool.h"

#include <cstring>
#include <memory>
#include <string>
#include "dummy_client.h"
#include "types.h"
#include <vector>
#include <unordered_map>

#include "aiges_type.h"

using namespace mooncake;

class bytes final {
   public:
    bytes();
    explicit bytes(char *_str, uint64_t str_len);

    bytes(const bytes &) = delete;
    bytes &operator=(const bytes &) = delete;

    bytes(bytes &&) noexcept = default;
    bytes &operator=(bytes &&) noexcept = default;

    const char *data() const;
    uint64_t size() const;

   private:
    std::shared_ptr<char> str;
    uint64_t length;
};

class KVConnector {
   public:
    KVConnector();
    ~KVConnector();

    int setup(const std::string &local_hostname, const std::string &metadata_server,
                const std::string &protocol, const std::string &rdma_devices,
                const std::string &master_server_addr,
              size_t global_segment_size, size_t local_buffer_size,const std::string &memory_allocator, wrapperMetrics cb = nullptr);

    int initAll(const std::string &local_hostname,const std::string &metadata_server,
                const std::string &protocol, const std::string &device_name,
                const std::string &master_server_addr,
               size_t mount_segment_size ,size_t buffer_allocator_size,const std::string &memory_allocator, wrapperMetrics cb = nullptr); 

    int put(const std::string &key, const std::string &value, size_t replica_num, const std::string &preferred_segment);

    bytes get(const std::string &key);

    int remove(const std::string &key);

    int exist(const std::string &key);

    std::vector<int> batchExist(const std::vector<std::string>& keys);

    int batchPut(
        const std::vector<std::string>& keys,
        const std::vector<std::string>& values,
        size_t replica_num,
        bool with_soft_pin,
        const std::string& preferred_segment
    );
    
    int batchPut(
        const std::vector<std::string>& keys,
        const std::vector<Slice>& values,
        size_t replica_num,
        bool with_soft_pin,
        const std::string& preferred_segment
    );

    int batchGet(const std::vector<std::string>& keys,std::unordered_map<std::string, mooncake::Slice>& result);
   
   private:
    std::unique_ptr<ThreadPoolManager> thread_pool_manager_;

    int allocateSlices(std::vector<mooncake::Slice> &slices, const std::string &value,
                      std::vector<std::unique_ptr<mooncake::AllocatedBuffer>>& buffers);
    
    // 替换为正确的元数据类型
    int allocateSlices(std::vector<mooncake::Slice> &slices, const std::vector<mooncake::Replica::Descriptor> &replica_descriptors,
                      uint64_t &length, std::vector<std::unique_ptr<mooncake::AllocatedBuffer>>& buffers);

    // 新增函数声明（支持原始数据指针）
    int allocateSlices(
        std::vector<mooncake::Slice>& slices, 
        const char* data_ptr,
        size_t data_len,
        std::vector<std::unique_ptr<mooncake::AllocatedBuffer>>& buffers
    );
    
    char *exportSlices(const std::vector<mooncake::Slice> &slices, uint64_t length);

    int freeSlices(std::vector<mooncake::Slice> &slices,
                  std::vector<std::unique_ptr<mooncake::AllocatedBuffer>>& buffers);

   public:
    std::shared_ptr<mooncake::Client> client_; 
    std::shared_ptr<mooncake::BufferAllocatorBase> client_buffer_allocator_ = nullptr;
    // uint64_t segment_ptr_;
    void* segment_ptr_ = nullptr;
    size_t global_segment_size_;
    std::string protocol;
    std::string device_name;
    std::string local_hostname;
    void* base_memory_ptr_ = nullptr;
};