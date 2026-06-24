/*
 * RNDISEthernetController.cpp — 以太网控制器实现（NetworkingDriverKit 版本）
 *
 * 继承自 IOUserNetworkEthernet，桥接 RNDIS USB 设备与
 * macOS 网络栈之间的数据通道。
 *
 * 核心职责：
 * - 创建 Packet Pool + TX/RX Submission Queue 并注册以太网接口
 * - 将 BULK IN 接收的以太网帧通过 RX Queue 投递到网络栈
 * - 通过 TX 定时器轮询 TX Queue 并通过 RNDIS 设备发送
 * - 管理多播模式、MAC 地址、MTU、媒体类型
 *
 * API 迁移：IOUserEthernetController (NetworkDriverKit) → IOUserNetworkEthernet (NetworkingDriverKit)
 */

#include "RNDISEthernetController.h"
#include "RNDISUSBDevice.h"

#include <string.h>

// ============================================================
// init() — 初始化
// ============================================================
bool RNDISEthernetController::init(void)
{
    os_log(OS_LOG_DEFAULT, "#RNDIS init()");

    if (!IOUserNetworkEthernet::init()) {
        return false;
    }

    fUSBDevice           = nullptr;
    fPacketPool          = nullptr;
    fRxQueue             = nullptr;
    fTxQueue             = nullptr;
    fTxDispatchQueue     = nullptr;
    fRxDispatchQueue     = nullptr;
    fEnabled             = false;
    fInterfaceRegistered = false;
    fMTU                 = 1500;

    memset(macAddress, 0, sizeof(macAddress));
    memset(&fMACAddress, 0, sizeof(fMACAddress));

    return true;
}

// ============================================================
// start() — 启动控制器
// ============================================================
/*
 * 步骤：
 *   1. 调用父类 start
 *   2. 绑定 RNDIS USB 设备
 *   3. 获取 MAC 地址
 *   4. 注册服务
 *
 * 注意：Packet Pool 和队列的创建延迟到 setInterfaceEnable 首次调用，
 *       因为 RNDIS 初始化完成后才会调用 setInterfaceEnable。
 */
bool RNDISEthernetController::start(IOService *provider)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "#RNDIS start()");

    if (!IOUserNetworkEthernet::start(provider)) {
        return false;
    }

    // 从 provider 获取 RNDISUSBDevice
    fUSBDevice = OSDynamicCast(RNDISUSBDevice, provider);
    if (!fUSBDevice) {
        os_log(OS_LOG_DEFAULT, "#RNDIS start(): provider is not RNDISUSBDevice");
        return false;
    }

    // 将自身注册到 USB 设备
    fUSBDevice->ethernetController = this;

    // 获取 MAC 地址
    ret = fetchMACAddress();
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "#RNDIS start(): fetchMACAddress failed: 0x%x", ret);
        return false;
    }

    os_log(OS_LOG_DEFAULT, "#RNDIS start(): MAC = %02x:%02x:%02x:%02x:%02x:%02x",
           macAddress[0], macAddress[1], macAddress[2],
           macAddress[3], macAddress[4], macAddress[5]);

    RegisterService();

    return true;
}

// ============================================================
// stop() — 停止控制器
// ============================================================
void RNDISEthernetController::stop(IOService *provider)
{
    os_log(OS_LOG_DEFAULT, "#RNDIS stop()");

    fEnabled   = false;
    fUSBDevice = nullptr;

    // 释放队列和 Pool
    OSSafeReleaseNULL(fTxQueue);
    OSSafeReleaseNULL(fRxQueue);
    OSSafeReleaseNULL(fTxDispatchQueue);
    OSSafeReleaseNULL(fRxDispatchQueue);
    OSSafeReleaseNULL(fPacketPool);

    IOUserNetworkEthernet::stop(provider);
}

// ============================================================
// free() — 释放资源
// ============================================================
void RNDISEthernetController::free(void)
{
    os_log(OS_LOG_DEFAULT, "#RNDIS free()");
}

// ============================================================
// createPacketPoolAndQueues — 创建 Pool 和队列
// ============================================================

// TX DequeueAction 回调：当网络栈有数据包需要发送时被调用
static uint32_t txDequeueCallback(
    OSObject                  *target,
    IOUserNetworkPacketQueue  *queue,
    IOUserNetworkPacket      **packetArray,
    uint32_t                   packetCount,
    void                      *refCon)
{
    (void)queue;
    (void)refCon;

    RNDISEthernetController *ctl = OSDynamicCast(RNDISEthernetController, target);
    if (!ctl) return 0;

    for (uint32_t i = 0; i < packetCount; i++) {
        IOUserNetworkPacket *pkt = packetArray[i];
        if (pkt && ctl->usbDevice() && ctl->usbDevice()->isReady()) {
            uint64_t dkva    = pkt->getDataVirtualAddress();
            size_t   offset  = pkt->getDataOff();
            size_t   len     = pkt->getDataLength();
            void    *dataPtr = reinterpret_cast<void *>(dkva + offset);
            ctl->usbDevice()->sendPacket(dataPtr, static_cast<uint32_t>(len));
        }
        if (pkt) pkt->release();
    }

    return packetCount;
}

/*
 * 流程：
 *   1. IOUserNetworkPacketBufferPool
 *   2. TX/RX Dispatch Queues
 *   3. TX Submission Queue (queueId=0)
 *   4. RX Submission Queue (queueId=1)
 *   5. IOTimerDispatchSource 用于 TX 轮询
 *   6. registerEthernetInterface
 *   7. reportLinkStatus
 */
IOReturn RNDISEthernetController::createPacketPoolAndQueues(void)
{
    IOReturn ret;

    os_log(OS_LOG_DEFAULT, "#RNDIS createPacketPoolAndQueues()");

    // 1. 创建 Packet Buffer Pool
    IOUserNetworkPacketBufferPoolOptions poolOpts = {};
    poolOpts.packetCount         = 512;
    poolOpts.bufferCount         = 512;
    poolOpts.bufferSize          = 2048;
    poolOpts.maxBuffersPerPacket = 1;
    poolOpts.memorySegmentSize   = 0;
    poolOpts.poolFlags           = PoolFlagSingleMemorySegment | PoolFlagMapToDext;

    ret = IOUserNetworkPacketBufferPool::CreateWithOptions(
        static_cast<IOService *>(this), "#RNDISPool", &poolOpts, &fPacketPool);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "#RNDIS CreateWithOptions failed: 0x%x", ret);
        return ret;
    }

    // 2. 创建 Dispatch Queues
    ret = IODispatchQueue::Create("#RNDIS-TxDQ",
        0, 0, &fTxDispatchQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "#RNDIS TxDispatchQueue Create failed: 0x%x", ret);
        return ret;
    }

    ret = IODispatchQueue::Create("#RNDIS-RxDQ",
        0, 0, &fRxDispatchQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "#RNDIS RxDispatchQueue Create failed: 0x%x", ret);
        return ret;
    }

    // 3. 创建 TX Submission Queue (queueId=0, with DequeueAction callback)
    fTxQueue = IOUserNetworkTxSubmissionQueue::withPool(
        fPacketPool, 256, 0, this,
        nullptr,               // QueryFreeSpaceAction
        txDequeueCallback,     // DequeueAction
        nullptr);              // refCon
    if (!fTxQueue) {
        os_log(OS_LOG_DEFAULT, "#RNDIS TxQueue withPool failed");
        return kIOReturnNoMemory;
    }

    // 4. 创建 RX Submission Queue (queueId=1)
    ret = IOUserNetworkRxSubmissionQueue::Create(
        fPacketPool, this, 256, 1, fRxDispatchQueue, &fRxQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "#RNDIS RxQueue Create failed: 0x%x", ret);
        return ret;
    }

    // 5. 注册以太网接口（NDK_24 签名，带 MAC）
    IOUserNetworkPacketQueue *queues[] = { fTxQueue, fRxQueue };
    ret = registerEthernetInterface(
        fMACAddress, queues, 2, fPacketPool, fPacketPool);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "#RNDIS registerEthernetInterface failed: 0x%x", ret);
        return ret;
    }

    // 7. 报告链路状态
    reportLinkStatus(kIOUserNetworkLinkStatusActive, getInitialMedia());

    os_log(OS_LOG_DEFAULT, "#RNDIS createPacketPoolAndQueues() done");
    return kIOReturnSuccess;
}

// ============================================================
// setInterfaceEnable — 启用/禁用接口
// ============================================================
/*
 * 首次调用 setInterfaceEnable(true) 时延迟创建 Pool + 队列并注册接口。
 * 后续调用仅控制 TX 定时器和链路状态。
 */
IOReturn RNDISEthernetController::setInterfaceEnable(bool enable)
{
    os_log(OS_LOG_DEFAULT, "#RNDIS setInterfaceEnable(%s)", enable ? "true" : "false");

    if (enable) {
        if (!fInterfaceRegistered) {
            kern_return_t ret = createPacketPoolAndQueues();
            if (ret != kIOReturnSuccess) {
                os_log(OS_LOG_DEFAULT, "#RNDIS setInterfaceEnable: createPacketPoolAndQueues failed: 0x%x", ret);
                return ret;
            }
            fInterfaceRegistered = true;
        }
        // 请求 TX queue 开始投递数据包
        if (fTxQueue) {
            fTxQueue->requestDequeue();
        }
        reportLinkStatus(kIOUserNetworkLinkStatusActive, getInitialMedia());
    } else {
        if (fTxQueue) {
            fTxQueue->SetEnable(false);
        }
        reportLinkStatus(kIOUserNetworkLinkStatusInactive, getInitialMedia());
    }

    fEnabled = enable;
    return kIOReturnSuccess;
}

// ============================================================
// receivePacket — 接收数据包（从 RNDIS 设备到达）
// ============================================================
/*
 * 由 RNDISUSBDevice::recvComplete() 调用。
 * 使用 Packet Pool 分配 packet → 拷贝数据 → 入队到 RX Submission Queue。
 */
kern_return_t RNDISEthernetController::receivePacket(
    const void *data, uint32_t data_len)
{
    if (!data || data_len == 0) {
        return kIOReturnBadArgument;
    }

    if (!fPacketPool || !fRxQueue) {
        return kIOReturnNotReady;
    }

    // 从 pool 分配 packet
    IOUserNetworkPacket *pkt = nullptr;
    IOReturn ret = fPacketPool->allocatePacket(&pkt, kIOUserNetworkNonBlocking);
    if (ret != kIOReturnSuccess || !pkt) {
        return kIOReturnNoResources;
    }

    // 获取数据缓冲区指针并写入数据
    uint64_t dkva = pkt->getDataVirtualAddress();
    pkt->setDataOff(0);
    pkt->setDataLength(data_len);
    pkt->setLinkHeaderLength(14);

    memcpy(reinterpret_cast<void *>(dkva), data, data_len);

    // 入队到 RX submission queue
    ret = fRxQueue->EnqueuePacket(pkt);
    if (ret != kIOReturnSuccess) {
        pkt->release();
        return ret;
    }

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

    memcpy(macAddress, fUSBDevice->macAddress, 6);

    bool valid = false;
    for (int i = 0; i < 6; i++) {
        if (macAddress[i] != 0x00) {
            valid = true;
            break;
        }
    }

    if (!valid) {
        os_log(OS_LOG_DEFAULT, "#RNDIS fetchMACAddress: invalid MAC (all zeros)");
        return kIOReturnUnsupported;
    }

    memcpy(fMACAddress.octet, macAddress, 6);
    return kIOReturnSuccess;
}

// ============================================================
// getSupportedMediaArray — 支持的媒体类型
// ============================================================
IOReturn RNDISEthernetController::getSupportedMediaArray(
    MediaWord *mediaArray, uint32_t *mediaCount)
{
    if (!mediaArray || !mediaCount) {
        return kIOReturnBadArgument;
    }
    mediaArray[0] = kIOUserNetworkMediaEthernetAuto;
    *mediaCount   = 1;
    return kIOReturnSuccess;
}

// ============================================================
// getInitialMedia — 初始媒体类型
// ============================================================
MediaWord RNDISEthernetController::getInitialMedia(void)
{
    return kIOUserNetworkMediaEthernetAuto;
}

// ============================================================
// handleChosenMedia — 处理媒体选择
// ============================================================
IOReturn RNDISEthernetController::handleChosenMedia(MediaWord chosenMedia)
{
    (void)chosenMedia;
    return kIOReturnSuccess;
}

// ============================================================
// setMaxTransferUnit — 设置 MTU
// ============================================================
IOReturn RNDISEthernetController::setMaxTransferUnit(uint32_t mtu)
{
    fMTU = mtu;
    return kIOReturnSuccess;
}

// ============================================================
// getMaxTransferUnit — 获取 MTU
// ============================================================
uint32_t RNDISEthernetController::getMaxTransferUnit(void)
{
    return fMTU;
}

// ============================================================
// getHardwareAddress — 获取硬件地址
// ============================================================
IOReturn RNDISEthernetController::getHardwareAddress(ether_addr_t *addr)
{
    if (!addr) {
        return kIOReturnBadArgument;
    }
    memcpy(addr->octet, macAddress, 6);
    return kIOReturnSuccess;
}

// ============================================================
// setHardwareAddress — 设置硬件地址
// ============================================================
IOReturn RNDISEthernetController::setHardwareAddress(ether_addr_t *addr)
{
    if (!addr) {
        return kIOReturnBadArgument;
    }
    memcpy(macAddress, addr->octet, 6);
    return kIOReturnSuccess;
}

// ============================================================
// setPromiscuousModeEnable — 设置混杂模式
// ============================================================
IOReturn RNDISEthernetController::setPromiscuousModeEnable(bool enable)
{
    (void)enable;
    return kIOReturnSuccess;
}

// ============================================================
// setAllMulticastModeEnable — 设置全多播模式
// ============================================================
IOReturn RNDISEthernetController::setAllMulticastModeEnable(bool enable)
{
    (void)enable;
    return kIOReturnSuccess;
}

// ============================================================
// setMulticastAddresses — 设置多播地址列表
// ============================================================
IOReturn RNDISEthernetController::setMulticastAddresses(
    const ether_addr_t *addresses, uint32_t count)
{
    (void)addresses;
    (void)count;
    if (fUSBDevice && fUSBDevice->isReady()) {
        fUSBDevice->rndisSetPacketFilter(RNDIS_DEFAULT_FILTER);
    }
    return kIOReturnSuccess;
}

// ============================================================
// getHardwareAssists — 获取硬件加速能力
// ============================================================
uint32_t RNDISEthernetController::getHardwareAssists(void)
{
    return 0;  // RNDIS 设备不支持硬件加速
}
