#ifndef __AIGES_KV_WRAPPER_H__
#define __AIGES_KV_WRAPPER_H__

#include "aiges_type.h"
#include <stddef.h> 

// 仅 C++ 编译器处理 extern "C" 块
#ifdef __cplusplus
extern "C" {
#endif


typedef struct MoonCakeClientConfig MoonCakeClientConfig;
struct MoonCakeClientConfig {
    const char* local_hostname;
    const char* metadata_server;
    const char* protocol;
    const char* master_server_addr;
    const char* device_name;
    size_t      global_segment_size;
    size_t      local_buffer_size;
    size_t      replica_num; // 全局初始化的副本数
    const char* log_level;   // 日志级别（"INFO"、"WARNING"、"ERROR"），默认INFO
    const char* log_dir;     // 日志目录（默认"./logs"）
    size_t      max_log_size;     // 单个日志文件最大大小（单位：MB，默认100）
    const char* memory_allocator; // 内存分配方式
    wrapperMetrics metrics_callback;
};

// 错误码枚举
// -1 创建客户端失败
// -2 分配 local_memory失败
// -3 RegisterLocalMemory 失败
// -4 分配global失败
// -5 MountSegment失败
typedef enum {
    // 操作成功
    KVCONNECTOR_SUCCESS = 0, // 请求成功 exist 存在
    // 操作异常
    KVCONNECTOR_ERR_BACKEND_OP = -1,   // put get del exist失败
    KVCONNECTOR_ERR_DATA_NOT_FOUND = -2, // get 未找到有效数据
    KVCONNECTOR_ERR_KEY_INVALID = -3, // key无效为空或空指针
    KVCONNECTOR_ERR_PARTIAL_EXIST = -4, // exist部分存在
    KVCONNECTOR_ERR_NOT_EXIST = -5, // exist全部不存在
    KVCONNECTOR_ERR_PARTIAL_DEL = -6, // del部分成功
    KVCONNECTOR_ERR_INVALID_PARAM = -7, // 参数无效
    KVCONNECTOR_ERR_MEMORY_ALLOC = -8,// 内存分配失败
    KVCONNECTOR_ERR_CONNECTOR_DESTROYED = -9, // kvconnector已销毁
    KVCONNECTOR_ERR_DATA_INVALID = -10, // key无效为空或空指针
    // 初始化异常
    KVCONNECTOR_ERR_INIT_FAILED = -20, // 初始化配置无效
    KVCONNECTOR_ERR_INIT_INVALID_PARAM = -21, // 初始化配置无效
    KVCONNECTOR_ERR_INIT_CREATE_CONNECTOR = -22, // 初始化创建 KVCONNECTOR失败
    KVCONNECTOR_ERR_INIT_CREATE_CLIENT = -23, // 初始化创建客户端错误
    KVCONNECTOR_ERR_INIT_ALLOC_LOCAL = -24, //初始化分配local内存失败
    KVCONNECTOR_ERR_INIT_REGIST_LOCAL = -25,// 初始化注册local失败
    KVCONNECTOR_ERR_INIT_ALLOC_GLOBAL = -26,//初始化分配global失败
    KVCONNECTOR_ERR_INIT_MOUNT_GLOBAL = -27,// 初始化mount global失败
    KVCONNECTOR_ERR_INIT_LOCAL_PARAM_INVALID = -28,// 初始化local内存参数无效
    KVCONNECTOR_ERR_INIT_LOG_DIR = -29, // 创建日志目录失败
} KVConnectorErrorCode;

WrapperAPI int init_mooncake_handler(CacheHandler* handler, const MoonCakeClientConfig* config);
WrapperAPI int destroy_mooncake_handler();

// 关闭 extern "C" 块（仅 C++ 编译器处理）
#ifdef __cplusplus
}
#endif

#endif