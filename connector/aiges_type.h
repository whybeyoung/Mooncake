#ifndef __AIGES_TYPE_H__
#define __AIGES_TYPE_H__

#include <stdlib.h>
#include <stdbool.h>

#define WrapperAPI __attribute__ ((visibility("default")))

typedef enum{
    CTMeterCustom =   0,      // 自定义计量接口
    CTMetricsLog  =   1,      // 自定义metrics日志接口
    CTTraceLog    =   2,      // 自定义trace日志接口
    CTLbExtra    =   4,      // 自定义负载均衡接口,
    CTCacheHandler    =   5,      // 自定义负载均衡接口,
} CtrlType;

typedef enum{
    DataText    =   0,      // 文本数据
    DataAudio   =   1,      // 音频数据
    DataImage   =   2,      // 图像数据
    DataVideo   =   3,      // 视频数据
    DataPer     =   4,      // 个性化数据
    DataRaw     =   4,      // 二进制流
} DataType;

typedef enum{
    DataBegin   =   0,      // 首数据
    DataContinue =  1,      // 中间数据
    DataEnd     =   2,      // 尾数据
    DataOnce    =   3,      // 非会话单次输入输出
} DataStatus;


typedef struct ParamList {
    char *key;
    char *value;
    unsigned int vlen;
    struct ParamList *next;
} *pParamList, *pConfig, *pDescList;     // 配置对复用该结构定义

typedef int(*wrapperMetrics)(const char* usrTag, const char *key, pParamList labels, int64_t value);

/**
 * 缓存配置结构体
 *
 * 用于封装缓存操作所需的配置信息。
 */
typedef struct CacheConfig {
    struct ParamList *desc; /**< 指向参数列表的指针，用于存储缓存相关的描述信息 */
} CacheConfig, *pCacheConfig;


typedef struct DataList {
    char *key;            // 数据标识
    void *data;           // 数据实体
    unsigned int len;       // 数据长度
    DataType type;       // 数据类型
    DataStatus status;      // 数据状态
    pDescList desc;         // 数据描述
    struct DataList *next;  // 链表指针
} *pDataList;

typedef enum {
    STRedis = 0, // redis
    STMooncake = 1, // mooncake storage
    STMysql = 2, // mysql
    STMongodb = 3, // mongodb
} StorageType;


/**
 * 键列表结构体
 *
 * 用于表示一个键链表节点，包含键值和存在状态信息。
 */
typedef struct KeyList {
    char *key;              /**< 键名称字符串指针 */
    bool is_exist;          /**< 键是否存在的标志 */
    struct KeyList *next;   /**< 指向下一个键列表节点的指针 */
} KeyList, *pKeyList;



/**
 * 缓存操作处理器结构体
 *
 * 用于封装不同存储后端（如 Redis、Mooncake、MongoDB 等）的操作接口。
 * 通过函数指针实现多态行为，支持多种存储类型统一调用。
 */
typedef struct {

    /**
     * 写入缓存（keys 和 inData 为输入参数）
     * @param keys 键列表指针。
     * @param inData 输入数据链表。
     * @param config 缓存配置结构体。
     * @return 操作结果状态码。
     */
    int (*put)(const KeyList *keys, pDataList inData, const CacheConfig *config);

    /**
     * 读取缓存（keys 为输入，outData 为输出参数需由 SDK 分配）
     * @param keys 键列表指针。
     * @param outData 数据链表指针的指针，用于返回读取到的数据。
     * @param config 缓存配置结构体。
     * @return 操作结果状态码。
     */
    int (*get)(const KeyList *keys, pDataList *outData, const CacheConfig *config);

    /**
     * 释放由 get 接口分配的内存
     * @param data 要释放的数据链表。
     * @return 操作结果状态码。
     */
    int (*freeDataList)(pDataList data);

    /**
     * 读取缓存到预分配内存（keys 和 ioData 均为输入参数）
     * @param keys 键列表指针。
     * @param ioData 输入/输出数据链表。
     * @param config 缓存配置结构体。
     * @return 操作结果状态码。 // todo 不实现
     */
    int (*getToMemory)(const KeyList *keys, pDataList ioData, const CacheConfig *config);

    /**
     * 删除缓存（keys 为输入参数）
     * @param keys 键列表指针。
     * @param config 缓存配置结构体。
     * @return 操作结果状态码。
     */
    int (*del)(const KeyList *keys, const CacheConfig *config);

    /**
     * 检查 key 是否存在（keys 为输入参数）
     * @param keys 键列表指针。
     * @param config 缓存配置结构体。
     * @return 操作结果状态码。
     */
    int (*exist)(pKeyList keys, const CacheConfig *config);

} CacheHandler;

#endif