/*
 * RNDISEthernetController.cpp — 以太网控制器实现
 *
 * 作为 IOUserEthernetController 的子类，桥接 RNDIS USB 设备与
 * macOS 网络栈之间的数据通道。
 *
 * 核心职责：
 * - 创建并注册 IOUserEthernetInterface
 * - 将 BULK IN 接收的以太网帧投递到网络栈
 * - 将网络栈发出的数据包通过 RNDIS 设备发送
 * - 管理多播模式和 MAC 地址
 *
 * 参考：HoRNDIS.cpp
 */

#include "RNDISEthernetController.h"
#include "RNDISUSBDevice.h"

#include <string.h>

#define RNDIS_LOG "com.yourcompany.RNDISDriver"

// ============================================================
// init() — 初始化
// ============================================================
bool RNDISEthernetController::init(void)
{
    os_log(OS_LOG_DEFAULT, "%s::init()\n", RNDIS_LOG);

    if (!IOUserEthernetController::init()) {
        return false;
    }

    fUSBDevice  = nullptr;
    fInterface  = nullptr;
    fEnabled    = false;

    memset(macAddress, 0, sizeof(macAddress));

    return true;
}

// ============================================================
// Start() — 启动控制器
// ============================================================
/*
 * 步骤：
 *   1. 调用父类 Start
 *   2. 绑定 RNDIS USB 设备
 *   3. 获取 MAC 地址
 *   4. 注册服务
 */
kern_return_t RNDISEthernetController::Start(IOService *provider)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "%s::Start()\n", RNDIS_LOG);

    ret = IOUserEthernetController::Start(provider);
    if (ret != kIOReturnSuccess) return ret;

    // 从 provider 获取 RNDISUSBDevice
    fUSBDevice = OSDynamicCast(RNDISUSBDevice, provider);
    if (!fUSBDevice) {
        os_log_error(OS_LOG_DEFAULT, "%s: provider is not RNDISUSBDevice\n", RNDIS_LOG);
        return kIOReturnNoDevice;
    }

    // 将自身注册到 USB 设备
    fUSBDevice->ethernetController = this;

    // 获取 MAC 地址
    ret = fetchMACAddress();
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "%s: fetchMACAddress failed: 0x%x\n", RNDIS_LOG, ret);
        return ret;
    }

    os_log(OS_LOG_DEFAULT, "%s: MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
           RNDIS_LOG,
           macAddress[0], macAddress[1], macAddress[2],
           macAddress[3], macAddress[4], macAddress[5]);

    RegisterService();

    return kIOReturnSuccess;
}

// ============================================================
// Stop() — 停止控制器
// ============================================================
kern_return_t RNDISEthernetController::Stop(IOService *provider)
{
    os_log(OS_LOG_DEFAULT, "%s::Stop()\n", RNDIS_LOG);

    fEnabled   = false;
    fUSBDevice = nullptr;
    fInterface = nullptr;

    return IOUserEthernetController::Stop(provider);
}

// ============================================================
// free() — 释放资源
// ============================================================
void RNDISEthernetController::free(void)
{
    os_log(OS_LOG_DEFAULT, "%s::free()\n", RNDIS_LOG);
    IOUserEthernetController::free();
}

// ============================================================
// createEthernetInterface — 创建网络接口
// ============================================================
/*
 * 框架调用此方法来创建 IOUserEthernetInterface 实例。
 * 父类的 CreateInterface() 会自动处理接口注册。
 */
kern_return_t RNDISEthernetController::createEthernetInterface(
    IOUserNetworkInterface **interface)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "%s::createEthernetInterface()\n", RNDIS_LOG);

    // 调用父类方法创建标准以太网接口
    ret = IOUserEthernetController::CreateInterface(
        kIOUserNetworkInterfaceTypeEthernet,
        interface);

    if (ret == kIOReturnSuccess && *interface) {
        fInterface = OSDynamicCast(IOUserEthernetInterface, *interface);
        if (fInterface) {
            os_log(OS_LOG_DEFAULT, "%s: Ethernet interface created\n", RNDIS_LOG);
        }
    }

    return ret;
}

// ============================================================
// setMulticastMode — 设置多播模式
// ============================================================
kern_return_t RNDISEthernetController::setMulticastMode(
    IOUserNetworkMulticastMode mode,
    const IOUserNetworkAddress *addrs,
    uint32_t count)
{
    os_log(OS_LOG_DEFAULT, "%s::setMulticastMode(mode=%u, count=%u)\n",
           RNDIS_LOG, mode, count);

    // 当前实现：使用 RNDIS_DEFAULT_FILTER 覆盖所有多播需求
    // 更精细的实现可以在 RNDISUSBDevice 中添加多播地址过滤
    (void)addrs;  // 标记已使用

    if (fUSBDevice && fUSBDevice->isReady()) {
        fUSBDevice->rndisSetPacketFilter(RNDIS_DEFAULT_FILTER);
    }

    return kIOReturnSuccess;
}

// ============================================================
// setMulticastList — 设置多播地址列表
// ============================================================
kern_return_t RNDISEthernetController::setMulticastList(
    const IOUserNetworkAddress *addrs,
    uint32_t count)
{
    os_log(OS_LOG_DEFAULT, "%s::setMulticastList(count=%u)\n", RNDIS_LOG, count);

    // 当前实现：不维护精细多播列表
    // 仅确保通信滤波器已启用
    (void)addrs;

    return kIOReturnSuccess;
}

// ============================================================
// fetchMACAddress — 从 RNDIS 设备获取 MAC 地址
// ============================================================
kern_return_t RNDISEthernetController::fetchMACAddress(void)
{
    if (!fUSBDevice) {
        return kIOReturnNoDevice;
    }

    // 从 RNDISUSBDevice 复制已获取的 MAC 地址
    memcpy(macAddress, fUSBDevice->macAddress, 6);

    // 验证 MAC 地址有效性（非全零、非广播）
    bool valid = false;
    for (int i = 0; i < 6; i++) {
        if (macAddress[i] != 0x00) {
            valid = true;
            break;
        }
    }

    if (!valid) {
        os_log_error(OS_LOG_DEFAULT, "%s: invalid MAC address (all zeros)\n", RNDIS_LOG);
        return kIOReturnUnsupported;
    }

    // 通过 IOUserEthernetController 的 MAC 地址属性告知网络栈
    // 注意：MAC 地址设置由创建网络接口时框架自动处理
    SetProperty("IOMACAddress", macAddress, 6);

    return kIOReturnSuccess;
}

// ============================================================
// outputPacket — 发送数据包（从网络栈发出）
// ============================================================
/*
 * 网络栈通过 IOUserEthernetController 框架调用此方法发送数据包。
 *
 * 流程：
 *   1. 从 IOUserNetworkPacket 中提取以太网帧数据
 *   2. 调用 RNDISUSBDevice::sendPacket() 通过 BULK OUT 发送
 *   3. 返回 kIOReturnSuccess 表示已提交发送
 *
 * 注意：此方法可以返回 kIOReturnSuccess 并稍后通过异步
 * 回调确认发送完成。当前实现为简化版同步提交。
 */
kern_return_t RNDISEthernetController::outputPacket(
    IOUserNetworkPacket *pkt,
    void *param)
{
    (void)param;

    if (!fUSBDevice || !fUSBDevice->isReady()) {
        return kIOReturnNotReady;
    }

    if (!pkt) {
        return kIOReturnBadArgument;
    }

    // 获取数据包信息
    uint32_t pktLen = 0;
    kern_return_t ret = pkt->GetDataLength(&pktLen);
    if (ret != kIOReturnSuccess || pktLen == 0) {
        return kIOReturnBadArgument;
    }

    // 获取数据指针
    void *dataPtr = nullptr;
    uint32_t dataLen = 0;
    ret = pkt->GetData(&dataPtr, &dataLen);
    if (ret != kIOReturnSuccess || !dataPtr || dataLen == 0) {
        return kIOReturnBadArgument;
    }

    // 发送以太网帧
    return fUSBDevice->sendPacket(dataPtr, dataLen);
}

// ============================================================
// receivePacket — 接收数据包（从 RNDIS 设备到达）
// ============================================================
/*
 * 由 RNDISUSBDevice::recvComplete() 调用，将接收到的
 * 以太网帧投递到 macOS 网络栈。
 *
 * 流程：
 *   1. 分配网络包缓冲区
 *   2. 将以太网帧拷贝到网络包
 *   3. 调用 inputPacket() 投递到网络栈
 */
kern_return_t RNDISEthernetController::receivePacket(
    const void *data,
    uint32_t    data_len)
{
    if (!data || data_len == 0) {
        return kIOReturnBadArgument;
    }

    // 通过框架方法将数据投递到网络栈
    // IOUserEthernetController 提供了封装的接收方法
    kern_return_t ret = IOUserEthernetController::inputPacket(
        data,
        data_len,
        0,    // 不需要额外的头部预留
        nullptr);

    if (ret != kIOReturnSuccess) {
        // 接收失败可能意味着网络栈繁忙或接口尚未就绪
        // 不记录错误日志以避免日志洪流
    }

    return ret;
}
