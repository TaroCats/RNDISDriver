/*
 * RNDISUSBDevice.h — USB 设备管理类（DriverKit 版本）
 *
 * 继承自 IOService，负责：
 * - USB 接口发现与匹配（Probe 阶段）
 * - 控制接口和数据接口的打开与管理
 * - RNDIS 命令通过 USB 控制传输通信
 * - 数据通道 BULK IN/OUT 异步传输
 *
 * 参考：HoRNDIS.cpp 中的 USB 通信逻辑
 */

#ifndef RNDISUSBDevice_h
#define RNDISUSBDevice_h

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <os/log.h>
#include "RNDISProtocol.h"

// ============================================================
// 缓冲区大小常量
// ============================================================
#define IN_BUF_SIZE        16384   // 接收缓冲区（MS-RNDIS 推荐 0x4000）
#define OUT_BUF_SIZE       4096    // 发送缓冲区（单个数据包）
#define N_OUT_BUFS         4       // 发送缓冲区环形数量
#define RNDIS_CMD_BUF_SZ   1024    // 控制命令缓冲区（0x400）
#define TRANSMIT_QUEUE_SIZE 256    // 发送队列深度

// ============================================================
// USB CDC 类定义
// ============================================================
#define USB_CDC_CLASS       0x02    // CDC 通信类
#define USB_CDC_DATA_CLASS  0x0A    // CDC 数据类
#define USB_MISC_CLASS      0xEF    // Misc 设备类
#define USB_VENDOR_SPECIFIC 0xFF    // 厂商特定

// CDC 子类
#define USB_CDC_ACM_SUBCLASS    0x02  // Abstract Control Model
#define USB_CDC_ETHERNET_SUBCLASS 0x06  // Ethernet Networking Control Model

// CDC 协议
#define USB_CDC_AT_PROTOCOL      0x01  // AT 命令协议（RNDIS over CDC）
#define USB_CDC_NCM_PROTOCOL     0x01  // Network Control Model

// USB CDC 类请求码
#define USB_CDC_SEND_ENCAPSULATED_COMMAND     0x00  // 发送封装命令
#define USB_CDC_GET_ENCAPSULATED_RESPONSE     0x01  // 获取封装响应

class RNDISEthernetController;

/*
 * RNDISUSBDevice — 管理 USB 设备的 RNDIS 通信
 *
 * 生命周期：
 *   Probe() → Start() → [正常运行] → Stop() → free()
 *
 * 接口架构：
 *   - 控制接口 (fCommInterface): 用于 RNDIS 命令（DeviceRequest 传输）
 *   - 数据接口 (fDataInterface): 用于以太网帧传输（BULK IN/OUT）
 */
class RNDISUSBDevice : public IOService
{
    OSDeclareDefaultStructors(RNDISUSBDevice);

public:
    // ========== IOService 生命周期 ==========
    virtual bool init(void) override;
    virtual kern_return_t Start(IOService *provider) override;
    virtual kern_return_t Stop(IOService *provider) override;
    virtual void free(void) override;

    // ========== Probe 匹配逻辑 ==========
    /*
     * 检查 USB 接口是否属于 RNDIS 设备。
     * 支持三种 RNDIS 设备类型：
     *   1. 标准 Android CDC: Class=224(E0h)/Sub=1/Proto=3
     *   2. Misc RNDIS over Ethernet: Class=239(EFh)/Sub=4/Proto=1
     *   3. Linux Gadget: Class=2(CDC)/Sub=2(ACM)/Proto=255(Vendor)
     */
    virtual kern_return_t Probe(void) override;

    // ========== RNDIS 控制通道 ==========
    /*
     * rndisCommand — 发送 RNDIS 命令并等待响应
     *
     * @param msg_type  消息类型
     * @param send_buf  发送缓冲区
     * @param send_len  发送长度
     * @param recv_buf  接收缓冲区
     * @param recv_len  接收缓冲区大小 / 实际接收长度
     * @return kIOReturnSuccess 或错误码
     */
    kern_return_t rndisCommand(uint32_t       msg_type,
                               const void    *send_buf,
                               uint32_t       send_len,
                               void          *recv_buf,
                               uint32_t      *recv_len);

    /*
     * rndisInit — 执行 RNDIS 初始化序列
     *
     * 发送 RNDIS_MSG_INIT → 接收 RNDIS_MSG_INIT_C
     * 获取设备的 max_transfer_size 等参数
     */
    kern_return_t rndisInit(void);

    /*
     * rndisQuery — 查询指定 OID
     *
     * @param oid       要查询的 OID
     * @param info_buf  查询结果缓冲区
     * @param info_len  缓冲区大小 / 实际数据长度
     * @return kIOReturnSuccess 或错误码
     */
    kern_return_t rndisQuery(uint32_t oid, void *info_buf, uint32_t *info_len);

    /*
     * rndisSetPacketFilter — 设置包过滤器
     *
     * @param filter  过滤器位掩码（RNDIS_PACKET_FILTER_*）
     */
    kern_return_t rndisSetPacketFilter(uint32_t filter);

    // ========== RNDIS 数据通道 ==========
    /*
     * sendPacket — 通过 BULK OUT 发送以太网帧
     *
     * 将 RNDIS 数据头 + 以太网帧通过 USB BULK OUT 异步发送。
     *
     * @param data     以太网帧数据
     * @param data_len 帧长度
     * @return kIOReturnSuccess 或错误码
     */
    kern_return_t sendPacket(const void *data, uint32_t data_len);

    /// 发送完成回调
    static void sendComplete(void *refcon, kern_return_t status,
                             IOBufferMemoryDescriptor *buffer,
                             uint64_t actualByteCount);

    /// 接收完成回调
    static void recvComplete(void *refcon, kern_return_t status,
                             IOBufferMemoryDescriptor *buffer,
                             uint64_t actualByteCount);

    // ========== 属性查询 ==========
    /// 设备是否已就绪可以收发数据
    bool isReady(void) const { return fReadyToTransfer; }

    /// MAC 地址（6 字节）
    uint8_t macAddress[6];

    /// 最大传输单元（max_transfer_size，不含 RNDIS 头）
    uint32_t maxTransferSize;

    /// 以太网控制器引用
    RNDISEthernetController *ethernetController;

private:
    // ========== USB 接口和管道 ==========
    IOUSBHostInterface  *fCommInterface;   // 控制接口
    IOUSBHostInterface  *fDataInterface;   // 数据接口
    IOUSBHostPipe       *fInPipe;          // BULK IN 管道
    IOUSBHostPipe       *fOutPipe;         // BULK OUT 管道

    // ========== 缓冲区 ==========
    IOBufferMemoryDescriptor *fInBuf;            // 接收缓冲区
    IOBufferMemoryDescriptor *fOutBufs[N_OUT_BUFS]; // 发送缓冲区环形数组
    int                      fOutBufIndex;       // 当前可用发送缓冲区索引

    // ========== 状态标志 ==========
    bool fReadyToTransfer;   // 设备已初始化完成，可传输数据
    bool fDataDead;          // 数据通道已中断
    uint8_t  fCommIfNum;     // 控制接口编号 (bInterfaceNumber)
    uint8_t  fDataIfNum;     // 数据接口编号

    // ========== 内部方法 ==========
    kern_return_t openInterfaces(IOUSBHostDevice *device);
    kern_return_t openPipes(void);
    kern_return_t startAsyncReceive(void);

    // 通过 USB 控制端点发送/接收 RNDIS 命令
    kern_return_t sendControlCommand(const void *buf, uint32_t len);
    kern_return_t recvControlResponse(void *buf, uint32_t *len);

    // 请求 ID 计数器
    uint32_t fRequestID;
};

#endif /* RNDISUSBDevice_h */
