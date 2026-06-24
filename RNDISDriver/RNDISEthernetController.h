/*
 * RNDISEthernetController.h — 以太网控制器（DriverKit 版本）
 *
 * 继承自 IOUserEthernetController，负责：
 * - 创建和管理网络接口
 * - 从 RNDIS 设备获取 MAC 地址并配置
 * - 数据包接收/发送的桥接
 * - 多播模式管理
 *
 * 参考：HoRNDIS.cpp 中的网络接口管理逻辑
 */

#ifndef RNDISEthernetController_h
#define RNDISEthernetController_h

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <NetworkDriverKit/IOUserEthernetController.h>
#include <NetworkDriverKit/IOUserEthernetInterface.h>
#include <os/log.h>

class RNDISUSBDevice;

/*
 * RNDISEthernetController — 网络控制器
 *
 * 作为 IOUserEthernetController 的子类，桥接 RNDIS USB 设备与
 * macOS 网络栈之间的数据通道。
 *
 * 架构：
 *   macOS 网络栈
 *       ↕
 *   IOUserEthernetInterface
 *       ↕
 *   RNDISEthernetController ←→ RNDISUSBDevice ←→ USB 设备
 */
class RNDISEthernetController : public IOUserEthernetController
{
    OSDeclareDefaultStructors(RNDISEthernetController);

public:
    // ========== IOService 生命周期 ==========
    virtual bool init(void) override;
    virtual kern_return_t Start(IOService *provider) override;
    virtual kern_return_t Stop(IOService *provider) override;
    virtual void free(void) override;

    // ========== IOUserEthernetController 必须实现的虚函数 ==========
    /*
     * createEthernetInterface — 创建并初始化网络接口
     *
     * 网络栈调用此方法创建 IOUserEthernetInterface 实例。
     */
    virtual kern_return_t createEthernetInterface(
        IOUserNetworkInterface **interface) override;

    /*
     * setMulticastMode — 设置多播模式
     *
     * @param mode    多播模式位掩码
     * @param addrs   多播地址列表
     * @param count   地址数量
     */
    virtual kern_return_t setMulticastMode(
        IOUserNetworkMulticastMode mode,
        const IOUserNetworkAddress *addrs,
        uint32_t count) override;

    /*
     * setMulticastList — 设置多播地址列表
     *
     * @param addrs  多播地址列表
     * @param count  地址数量
     */
    virtual kern_return_t setMulticastList(
        const IOUserNetworkAddress *addrs,
        uint32_t count) override;

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

    /*
     * 从网络栈发送数据包
     *
     * IOUserEthernetController 框架调用此方法。
     *
     * @param pkt    网络数据包
     * @param param  附加参数
     */
    virtual kern_return_t outputPacket(IOUserNetworkPacket *pkt,
                                       void *param) override;

    // ========== 设备绑定 ==========
    /// 绑定 RNDIS USB 设备
    void setUSBDevice(RNDISUSBDevice *device) { fUSBDevice = device; }

    /// 获取 RNDIS USB 设备
    RNDISUSBDevice *usbDevice(void) const { return fUSBDevice; }

    // ========== MAC 地址管理 ==========
    /// 从 RNDIS 设备获取并设置 MAC 地址
    kern_return_t fetchMACAddress(void);

    /// 已获取的 MAC 地址（6 字节）
    uint8_t macAddress[6];

private:
    RNDISUSBDevice  *fUSBDevice;       // RNDIS USB 设备
    IOUserEthernetInterface *fInterface; // 网络接口
    bool             fEnabled;          // 接口是否已启用
};

#endif /* RNDISEthernetController_h */
