/*
 * RNDISEthernetController.h — 以太网控制器（DriverKit 版本）
 *
 * 继承自 IOUserNetworkEthernet (NetworkingDriverKit)，负责：
 * - 创建和管理网络接口（Packet Pool + Packet Queue 架构）
 * - 从 RNDIS 设备获取 MAC 地址并配置
 * - 数据包接收/发送的桥接
 * - 多播模式管理
 *
 * 参考：HoRNDIS.cpp 中的网络接口管理逻辑
 * 迁移自：IOUserEthernetController (NetworkDriverKit) → IOUserNetworkEthernet (NetworkingDriverKit)
 */

#ifndef RNDISEthernetController_h
#define RNDISEthernetController_h

#define DRIVERKIT_FIX_OSTYPEID 1

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <NetworkingDriverKit/IOUserNetworkEthernet.h>
#include <NetworkingDriverKit/IOUserNetworkPacket.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>
#include <NetworkingDriverKit/IOUserNetworkPacketQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTypes.h>
#include <os/log.h>

class RNDISUSBDevice;

/*
 * RNDISEthernetController — 网络控制器
 *
 * 作为 IOUserNetworkEthernet 的子类，桥接 RNDIS USB 设备与
 * macOS 网络栈之间的数据通道。
 *
 * 架构：
 *   macOS 网络栈
 *       ↕
 *   IOUserNetworkEthernet (Packet Pool + TX/RX Queue)
 *       ↕
 *   RNDISEthernetController ←→ RNDISUSBDevice ←→ USB 设备
 */
class RNDISEthernetController : public IOUserNetworkEthernet
{
public:
    // ========== IOService 生命周期 ==========
    virtual bool init(void) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free(void) override;

    // ========== IOUserNetworkEthernet 必须实现的虚函数 ==========
    virtual IOReturn setInterfaceEnable(bool enable) override;
    virtual IOReturn setPromiscuousModeEnable(bool enable) override;
    virtual IOReturn setAllMulticastModeEnable(bool enable) override;
    virtual IOReturn handleChosenMedia(MediaWord chosenMedia) override;
    virtual IOReturn setMaxTransferUnit(uint32_t mtu) override;
    virtual uint32_t getMaxTransferUnit() override;
    virtual IOReturn getHardwareAddress(ether_addr_t *addr) override;
    virtual IOReturn setHardwareAddress(ether_addr_t *addr) override;
    virtual IOReturn getSupportedMediaArray(MediaWord *mediaArray, uint32_t *mediaCount) override;
    virtual IOReturn setMulticastAddresses(const ether_addr_t *addresses, uint32_t count) override;
    virtual uint32_t getHardwareAssists() override;
    virtual MediaWord getInitialMedia() override;

    // ========== 数据包处理 ==========
    /*
     * 接收数据包并投递到网络栈
     *
     * 由 RNDISUSBDevice 在接收到 BULK IN 数据后调用。
     *
     * @param data     以太网帧数据（不含 RNDIS 数据头）
     * @param data_len 帧长度
     * @return kIOReturnSuccess 或错误码
     */
    kern_return_t receivePacket(const void *data, uint32_t data_len);

    // ========== 设备绑定 ==========
    /// 绑定 RNDIS USB 设备
    void setUSBDevice(RNDISUSBDevice *device) { fUSBDevice = device; }

    /// 获取 RNDIS USB 设备
    RNDISUSBDevice *usbDevice(void) const { return fUSBDevice; }

    // ========== MAC 地址管理 ==========
    /// 从 RNDIS 设备获取并设置 MAC 地址
    kern_return_t fetchMACAddress(void);

    /// 已获取的 MAC 地址（6 字节，保留用于与 RNDISUSBDevice 交互）
    uint8_t macAddress[6];

private:
    // ========== 内部方法 ==========
    /// 首次启用接口时创建 Packet Pool、队列并注册以太网接口
    IOReturn createPacketPoolAndQueues(void);

    // ========== 成员变量 ==========
    RNDISUSBDevice                     *fUSBDevice;
    IOUserNetworkPacketBufferPool      *fPacketPool;
    IOUserNetworkRxSubmissionQueue     *fRxQueue;
    IOUserNetworkTxSubmissionQueue     *fTxQueue;
    IODispatchQueue                    *fTxDispatchQueue;
    IODispatchQueue                    *fRxDispatchQueue;
    bool                                fEnabled;
    bool                                fInterfaceRegistered;
    uint32_t                            fMTU;
    ether_addr_t                        fMACAddress;
};

#endif /* RNDISEthernetController_h */
