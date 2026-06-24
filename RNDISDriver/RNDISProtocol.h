/*
 * RNDISProtocol.h — RNDIS 协议定义 (DriverKit/Network Extension 版本)
 *
 * 参考：HoRNDIS.h、[MS-RNDIS] 规范
 * 从 kext 版本迁移到 DriverKit 环境，协议数据结构保持一致。
 *
 * 所有结构体按 little-endian 对齐，遵循 MS-RNDIS 规范。
 */

#ifndef RNDISProtocol_h
#define RNDISProtocol_h

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>

#pragma pack(push, 1)

// ============================================================
// RNDIS 消息类型枚举
// ============================================================
enum {
    RNDIS_MSG_PACKET       = 0x00000001,  // 数据包（数据通道）
    RNDIS_MSG_INIT         = 0x00000002,  // 初始化请求 (Host → Device)
    RNDIS_MSG_INIT_C       = 0x80000002,  // 初始化完成 (Device → Host)
    RNDIS_MSG_HALT         = 0x00000003,  // 终止设备
    RNDIS_MSG_QUERY        = 0x00000004,  // OID 查询请求
    RNDIS_MSG_QUERY_C      = 0x80000004,  // OID 查询响应
    RNDIS_MSG_SET          = 0x00000005,  // OID 设置请求
    RNDIS_MSG_SET_C        = 0x80000005,  // OID 设置响应
    RNDIS_MSG_RESET        = 0x00000006,  // 重置请求
    RNDIS_MSG_RESET_C      = 0x80000006,  // 重置响应
    RNDIS_MSG_INDICATE     = 0x00000007,  // 设备主动通知 (Device → Host)
    RNDIS_MSG_KEEPALIVE    = 0x00000008,  // 心跳请求
    RNDIS_MSG_KEEPALIVE_C  = 0x80000008,  // 心跳响应
};

// 完成标志位：所有响应消息 = 请求消息 | RNDIS_MSG_COMPLETION
#define RNDIS_MSG_COMPLETION  0x80000000

// ============================================================
// RNDIS 状态码
// ============================================================
#define RNDIS_STATUS_SUCCESS   0x00000000
#define RNDIS_STATUS_FAILURE   0xC0000001

// ============================================================
// OID (Object Identifier) 定义
// ============================================================
#define OID_802_3_PERMANENT_ADDRESS    0x01010101  // 获取 MAC 地址
#define OID_GEN_MAXIMUM_FRAME_SIZE     0x00010106  // 最大帧大小
#define OID_GEN_CURRENT_PACKET_FILTER  0x0001010E  // 设置包过滤器
#define OID_GEN_PHYSICAL_MEDIUM        0x00010202  // 物理介质类型

// ============================================================
// 包过滤器位掩码
// ============================================================
#define RNDIS_PACKET_FILTER_DIRECTED       (1 << 0)   // 单播（定向）
#define RNDIS_PACKET_FILTER_MULTICAST      (1 << 1)   // 多播
#define RNDIS_PACKET_FILTER_ALL_MULTICAST  (1 << 2)   // 所有多播
#define RNDIS_PACKET_FILTER_BROADCAST      (1 << 3)   // 广播
#define RNDIS_PACKET_FILTER_PROMISCUOUS    (1 << 5)   // 混杂模式

// 默认过滤器：直接 + 广播 + 多播
#define RNDIS_DEFAULT_FILTER \
    (RNDIS_PACKET_FILTER_DIRECTED | \
     RNDIS_PACKET_FILTER_BROADCAST | \
     RNDIS_PACKET_FILTER_ALL_MULTICAST | \
     RNDIS_PACKET_FILTER_PROMISCUOUS)

// ============================================================
// RNDIS 通用消息头（所有 RNDIS 消息的公共前缀）
// ============================================================
typedef struct rndis_msg_hdr {
    uint32_t msg_type;      // 消息类型
    uint32_t msg_len;       // 消息总长度（含头）
    uint32_t request_id;    // 请求 ID（用于请求-响应匹配）
    uint32_t status;        // 状态码（仅响应消息有效）
} __attribute__((packed)) rndis_msg_hdr;

// ============================================================
// RNDIS 数据包头（数据通道中每个以太网帧的前缀）
// ============================================================
typedef struct rndis_data_hdr {
    uint32_t msg_type;            // RNDIS_MSG_PACKET
    uint32_t msg_len;             // 消息总长度
    uint32_t data_offset;         // 从消息头起始到真实数据的偏移
    uint32_t data_len;            // 真实数据长度
    uint32_t oob_data_offset;     // OOB 数据偏移
    uint32_t oob_data_len;        // OOB 数据长度
    uint32_t num_oob;             // OOB 数据元素个数
    uint32_t packet_data_offset;  // 每包数据偏移
    uint32_t packet_data_len;     // 每包数据长度
    uint32_t vc_handle;           // 虚通道句柄
    uint32_t reserved;            // 保留
} __attribute__((packed)) rndis_data_hdr;

// ============================================================
// RNDIS 初始化消息 (Host → Device)
// ============================================================
typedef struct rndis_init {
    uint32_t msg_type;           // RNDIS_MSG_INIT
    uint32_t msg_len;            // sizeof(rndis_init)
    uint32_t request_id;         // 请求 ID
    uint32_t major_version;      // 主版本号（通常为 1）
    uint32_t minor_version;      // 次版本号（通常为 0）
    uint32_t max_transfer_size;  // 主机支持的最大单次传输字节数
} __attribute__((packed)) rndis_init;

// ============================================================
// RNDIS 初始化完成消息 (Device → Host)
// ============================================================
typedef struct rndis_init_c {
    uint32_t msg_type;                // RNDIS_MSG_INIT_C
    uint32_t msg_len;                 // sizeof(rndis_init_c)
    uint32_t request_id;              // 请求 ID（与 INIT 的 request_id 对应）
    uint32_t status;                  // 状态码
    uint32_t major_version;           // 设备支持的 RNDIS 主版本
    uint32_t minor_version;           // 设备支持的 RNDIS 次版本
    uint32_t device_flags;            // 设备标志
    uint32_t medium;                  // 介质类型（0 = 802.3 Ethernet）
    uint32_t max_packets_per_transfer;// 每次传输最大包数
    uint32_t max_transfer_size;       // ★ 设备允许的最大单次传输字节数
    uint32_t packet_alignment;        // 包对齐边界
    uint32_t af_list_offset;          // 地址族列表偏移
    uint32_t af_list_size;            // 地址族列表大小
} __attribute__((packed)) rndis_init_c;

// ============================================================
// RNDIS 查询消息 (Host → Device)
// ============================================================
typedef struct rndis_query {
    uint32_t msg_type;          // RNDIS_MSG_QUERY
    uint32_t msg_len;           // sizeof(rndis_query) + info_buflen
    uint32_t request_id;        // 请求 ID
    uint32_t oid;               // 要查询的 OID
    uint32_t info_buflen;       // 信息缓冲区长度
    uint32_t info_bufoff;       // 信息缓冲区偏移
    uint32_t dev_vc_handle;     // 设备虚通道句柄
} __attribute__((packed)) rndis_query;

// ============================================================
// RNDIS 查询响应消息 (Device → Host)
// ============================================================
typedef struct rndis_query_c {
    uint32_t msg_type;          // RNDIS_MSG_QUERY_C
    uint32_t msg_len;           // sizeof(rndis_query_c) + info_buflen
    uint32_t request_id;        // 请求 ID
    uint32_t status;            // 状态码
    uint32_t info_buflen;       // 信息缓冲区长度
    uint32_t info_bufoff;       // 信息缓冲区偏移（从本结构起始位置）
} __attribute__((packed)) rndis_query_c;

// ============================================================
// RNDIS 设置消息 (Host → Device)
// ============================================================
typedef struct rndis_set {
    uint32_t msg_type;          // RNDIS_MSG_SET
    uint32_t msg_len;           // sizeof(rndis_set) + info_buflen
    uint32_t request_id;        // 请求 ID
    uint32_t oid;               // 要设置的 OID
    uint32_t info_buflen;       // 信息缓冲区长度
    uint32_t info_bufoff;       // 信息缓冲区偏移
    uint32_t dev_vc_handle;     // 设备虚通道句柄
} __attribute__((packed)) rndis_set;

// ============================================================
// RNDIS 设置响应消息 (Device → Host)
// ============================================================
typedef struct rndis_set_c {
    uint32_t msg_type;          // RNDIS_MSG_SET_C
    uint32_t msg_len;           // sizeof(rndis_set_c)
    uint32_t request_id;        // 请求 ID
    uint32_t status;            // 状态码
} __attribute__((packed)) rndis_set_c;

#pragma pack(pop)

#endif /* RNDISProtocol_h */
