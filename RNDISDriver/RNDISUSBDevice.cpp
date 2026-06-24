/*
 * RNDISUSBDevice.cpp — USB 设备管理实现
 *
 * 实现 RNDIS 协议下的 USB 设备管理，包括：
 * - iOService 生命周期管理
 * - USB 接口匹配和设备发现
 * - RNDIS 命令通信（控制传输）
 * - 数据收发（BULK 传输）
 *
 * 参考：HoRNDIS.cpp
 */

#include "RNDISUSBDevice.h"
#include "RNDISEthernetController.h"

#include <string.h>

// 日志子系统标识
#define RNDIS_LOG "com.yourcompany.RNDISDriver"

// ============================================================
// 宏定义：IOReturn → kern_return_t 简化检查
// ============================================================
#define CHECK(expr) do { \
    kern_return_t __ret = (expr); \
    if (__ret != kIOReturnSuccess) { \
        os_log(OS_LOG_DEFAULT, "%s:%d: %s failed: 0x%x", \
                     __FILE__, __LINE__, #expr, __ret); \
        return __ret; \
    } \
} while(0)

// ============================================================
// IOService 元类声明
// ============================================================
// DriverKit 25.5: 元类由 IIG 系统自动处理，无需手动声明

// ============================================================
// Probe() — USB 接口匹配检查
// ============================================================
/*
 * 当 USB 设备插入时，系统根据 Info.plist 中的 IOKitPersonalities
 * 匹配规则调用本方法。需要进一步检查接口参数以确认是 RNDIS 设备。
 *
 * 支持三种 RNDIS 设备类型：
 *   1. 标准 Android CDC (Class=E0h, SubClass=01h, Protocol=03h)
 *   2. Misc RNDIS over Ethernet (Class=EFh, SubClass=04h, Protocol=01h)
 *   3. Linux Gadget CDC ACM (Class=02h, SubClass=02h, Protocol=FFh)
 *
 * 匹配条件：控制接口符合上述类型之一，且其后紧跟 CDC Data 接口
 */
kern_return_t RNDISUSBDevice::probe(IOService *provider)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "%s: Probe() called\n", RNDIS_LOG);

    // 获取 provider（应为 IOUSBHostInterface）
    IOUSBHostInterface *iface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!iface) {
        os_log(OS_LOG_DEFAULT, "%s: provider is not IOUSBHostInterface\n", RNDIS_LOG);
        return kIOReturnUnsupported;
    }

    // 读取接口描述符 (DriverKit 25.5: 使用 GetInterfaceDescriptor)
    uint8_t  ifClass, ifSubClass, ifProtocol;
    uint16_t ifNum;

    const IOUSBInterfaceDescriptor *ifDesc = iface->GetInterfaceDescriptor(nullptr);
    if (!ifDesc) return kIOReturnUnsupported;

    ifClass    = ifDesc->bInterfaceClass;
    ifSubClass = ifDesc->bInterfaceSubClass;
    ifProtocol = ifDesc->bInterfaceProtocol;
    ifNum      = ifDesc->bInterfaceNumber;

    os_log(OS_LOG_DEFAULT, "%s: Probe interface %u: class=%02x subclass=%02x proto=%02x\n",
           RNDIS_LOG, ifNum, ifClass, ifSubClass, ifProtocol);

    // 判断是否为 RNDIS 控制接口
    bool isRNDIS = false;

    // 类型 1: 标准 Android CDC (E0/01/03)
    if (ifClass == 0xE0 && ifSubClass == 0x01 && ifProtocol == 0x03) {
        os_log(OS_LOG_DEFAULT, "%s: Matched Android CDC RNDIS\n", RNDIS_LOG);
        isRNDIS = true;
    }
    // 类型 2: Misc RNDIS over Ethernet (EF/04/01)
    else if (ifClass == 0xEF && ifSubClass == 0x04 && ifProtocol == 0x01) {
        os_log(OS_LOG_DEFAULT, "%s: Matched Misc RNDIS over Ethernet\n", RNDIS_LOG);
        isRNDIS = true;
    }
    // 类型 3: Linux Gadget (02/02/FF)
    else if (ifClass == 0x02 && ifSubClass == 0x02 && ifProtocol == 0xFF) {
        os_log(OS_LOG_DEFAULT, "%s: Matched Linux Gadget RNDIS\n", RNDIS_LOG);
        isRNDIS = true;
    }

    if (!isRNDIS) {
        return kIOReturnUnsupported;
    }

    // 保存控制接口编号，Start() 中使用
    fCommIfNum = ifNum;
    fDataIfNum = ifNum + 1;

    os_log(OS_LOG_DEFAULT, "%s: Probe success — commIf=%u, dataIf=%u\n",
           RNDIS_LOG, fCommIfNum, fDataIfNum);

    return kIOReturnSuccess;
}

// ============================================================
// init() — 初始化实例变量
// ============================================================
bool RNDISUSBDevice::init(void)
{
    os_log(OS_LOG_DEFAULT, "%s: init()\n", RNDIS_LOG);

    if (!IOService::init()) {
        return false;
    }

    // 初始化所有指针和状态
    fCommInterface    = nullptr;
    fDataInterface    = nullptr;
    fInPipe           = nullptr;
    fOutPipe          = nullptr;
    fInBuf            = nullptr;
    fReadyToTransfer  = false;
    fDataDead         = false;
    fCommIfNum        = 0;
    fDataIfNum        = 0;
    fOutBufIndex      = 0;
    fRequestID        = 0;
    maxTransferSize   = 0;
    ethernetController = nullptr;

    memset(macAddress, 0, sizeof(macAddress));
    memset(fOutBufs, 0, sizeof(fOutBufs));

    return true;
}

// ============================================================
// Start() — 启动设备服务
// ============================================================
/*
 * 在 Probe() 确认匹配后调用。
 * 步骤：
 *   1. 打开控制接口和数据接口
 *   2. 打开 BULK IN/OUT 管道
 *   3. 分配接收/发送缓冲区
 *   4. 执行 RNDIS 初始化序列
 *   5. 启动异步接收
 */
kern_return_t RNDISUSBDevice::Start(IOService *provider)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "%s: Start()\n", RNDIS_LOG);

    ret = IOService::Start(provider);
    if (ret != kIOReturnSuccess) return ret;

    // 获取 IOUSBHostDevice（从接口向上找到设备）
    IOUSBHostDevice *device = OSDynamicCast(IOUSBHostDevice, provider);
    if (!device) {
        os_log(OS_LOG_DEFAULT, "%s: cannot find IOUSBHostDevice\n", RNDIS_LOG);
        return kIOReturnNoDevice;
    }

    // 步骤 1: 打开 USB 接口
    ret = openInterfaces(device);
    if (ret != kIOReturnSuccess) return ret;

    // 步骤 2: 打开管道
    ret = openPipes();
    if (ret != kIOReturnSuccess) return ret;

    // 步骤 3: 分配接收缓冲区
    ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionInOut,
        IN_BUF_SIZE,
        0,
        &fInBuf);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: IOBufferMemoryDescriptor::Create(in) failed\n", RNDIS_LOG);
        return ret;
    }

    // 分配发送缓冲区（环形）
    for (int i = 0; i < N_OUT_BUFS; i++) {
        ret = IOBufferMemoryDescriptor::Create(
            kIOMemoryDirectionInOut,
            OUT_BUF_SIZE,
            0,
            &fOutBufs[i]);
        if (ret != kIOReturnSuccess) {
            os_log(OS_LOG_DEFAULT, "%s: IOBufferMemoryDescriptor::Create(out[%d]) failed\n",
                         RNDIS_LOG, i);
            return ret;
        }
    }

    // 步骤 4: RNDIS 初始化序列
    ret = rndisInit();
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: rndisInit() failed: 0x%x\n", RNDIS_LOG, ret);
        return ret;
    }

    // 获取 MAC 地址
    uint32_t macLen = 6;
    ret = rndisQuery(OID_802_3_PERMANENT_ADDRESS, macAddress, &macLen);
    if (ret != kIOReturnSuccess || macLen != 6) {
        os_log(OS_LOG_DEFAULT, "%s: rndisQuery(MAC) failed: 0x%x\n", RNDIS_LOG, ret);
        return ret;
    }

    os_log(OS_LOG_DEFAULT, "%s: MAC address = %02x:%02x:%02x:%02x:%02x:%02x\n",
           RNDIS_LOG,
           macAddress[0], macAddress[1], macAddress[2],
           macAddress[3], macAddress[4], macAddress[5]);

    // 设置包过滤器
    ret = rndisSetPacketFilter(RNDIS_DEFAULT_FILTER);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: rndisSetPacketFilter() failed: 0x%x\n", RNDIS_LOG, ret);
        return ret;
    }

    // 标记设备就绪
    fReadyToTransfer = true;

    // 步骤 5: 启动异步接收
    ret = startAsyncReceive();
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: startAsyncReceive() failed: 0x%x\n", RNDIS_LOG, ret);
        return ret;
    }

    os_log(OS_LOG_DEFAULT, "%s: Start() completed successfully\n", RNDIS_LOG);

    // 注册服务，让系统发现此设备
    RegisterService();

    return kIOReturnSuccess;
}

// ============================================================
// Stop() — 停止设备服务
// ============================================================
kern_return_t RNDISUSBDevice::Stop(IOService *provider)
{
    os_log(OS_LOG_DEFAULT, "%s: Stop()\n", RNDIS_LOG);

    fReadyToTransfer = false;
    fDataDead        = true;

    // 关闭管道（DriverKit 在 Close() 时自动清理）
    if (fInPipe) {
        fInPipe->Abort(0, kIOReturnAborted, nullptr);
        fInPipe->release();
        fInPipe = nullptr;
    }
    if (fOutPipe) {
        fOutPipe->Abort(0, kIOReturnAborted, nullptr);
        fOutPipe->release();
        fOutPipe = nullptr;
    }

    // 释放接口
    if (fCommInterface) {
        fCommInterface->Close(provider, 0);
        fCommInterface->release();
        fCommInterface = nullptr;
    }
    if (fDataInterface) {
        fDataInterface->Close(provider, 0);
        fDataInterface->release();
        fDataInterface = nullptr;
    }

    // 释放缓冲区
    if (fInBuf) {
        fInBuf->release();
        fInBuf = nullptr;
    }
    for (int i = 0; i < N_OUT_BUFS; i++) {
        if (fOutBufs[i]) {
            fOutBufs[i]->release();
            fOutBufs[i] = nullptr;
        }
    }

    return IOService::Stop(provider);
}

// ============================================================
// free() — 释放资源
// ============================================================
void RNDISUSBDevice::free(void)
{
    os_log(OS_LOG_DEFAULT, "%s: free()\n", RNDIS_LOG);
    IOService::free();
}

// ============================================================
// openInterfaces — 打开控制接口和数据接口
// ============================================================
kern_return_t RNDISUSBDevice::openInterfaces(IOUSBHostDevice *device)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "%s: Opening interfaces — comm=%u, data=%u\n",
           RNDIS_LOG, fCommIfNum, fDataIfNum);

    // 获取当前配置 (DriverKit 25.5: CopyConfigurationDescriptor 返回指针)
    const IOUSBConfigurationDescriptor *configDesc = device->CopyConfigurationDescriptor((uint8_t)0);
    if (!configDesc) {
        os_log(OS_LOG_DEFAULT, "%s: CopyConfigurationDescriptor failed\n", RNDIS_LOG);
        return kIOReturnNoDevice;
    }

    // 打开控制接口
    // DriverKit 中需要使用 CopyInterface() 或遍历接口
    // 这里使用框架提供的接口打开方法
    IOUSBHostInterface *commIface = nullptr;
    ret = device->CopyInterface(fCommIfNum, &commIface);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: CopyInterface(comm=%u) failed: 0x%x\n",
                     RNDIS_LOG, fCommIfNum, ret);
        return ret;
    }

    ret = commIface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: Open(comm) failed: 0x%x\n", RNDIS_LOG, ret);
        commIface->release();
        return ret;
    }
    fCommInterface = commIface;

    // 打开数据接口
    IOUSBHostInterface *dataIface = nullptr;
    ret = device->CopyInterface(fDataIfNum, &dataIface);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: CopyInterface(data=%u) failed: 0x%x\n",
                     RNDIS_LOG, fDataIfNum, ret);
        return ret;
    }

    ret = dataIface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: Open(data) failed: 0x%x\n", RNDIS_LOG, ret);
        dataIface->release();
        return ret;
    }
    fDataInterface = dataIface;

    os_log(OS_LOG_DEFAULT, "%s: Interfaces opened successfully\n", RNDIS_LOG);
    return kIOReturnSuccess;
}

// ============================================================
// openPipes — 打开 BULK IN/OUT 管道
// ============================================================
kern_return_t RNDISUSBDevice::openPipes(void)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "%s: Opening pipes\n", RNDIS_LOG);

    // 遍历数据接口的端点描述符，找到 BULK IN 和 BULK OUT (DriverKit 25.5)
    // 获取接口描述符并遍历其后的端点描述符
    const IOUSBInterfaceDescriptor *ifDesc = fDataInterface->GetInterfaceDescriptor(nullptr);
    if (!ifDesc) {
        os_log(OS_LOG_DEFAULT, "%s: GetInterfaceDescriptor failed\n", RNDIS_LOG);
        return kIOReturnNoDevice;
    }

    // 端点描述符紧跟在接口描述符之后
    const IOUSBEndpointDescriptor *epDesc = (const IOUSBEndpointDescriptor *)(ifDesc + 1);
    uint32_t epIndex = 0;

    // 遍历端点描述符
    while (epIndex < ifDesc->bNumEndpoints) {
        uint8_t epAddr    = epDesc->bEndpointAddress;
        uint8_t epType    = epDesc->bmAttributes & 0x03;  // 传输类型
        uint8_t epDir     = epAddr & 0x80;                // 方向: IN=0x80, OUT=0x00

        if (epType == kIOUSBEndpointTypeBulk) {
            if (epDir == 0x80) {
                // BULK IN — 设备 → 主机
                os_log(OS_LOG_DEFAULT, "%s: Found BULK IN endpoint: 0x%02x\n",
                       RNDIS_LOG, epAddr);

                ret = fDataInterface->CopyPipe(epAddr, &fInPipe);
                if (ret != kIOReturnSuccess) {
                    os_log(OS_LOG_DEFAULT, "%s: CopyPipe(IN) failed: 0x%x\n", RNDIS_LOG, ret);
                    return ret;
                }
            } else {
                // BULK OUT — 主机 → 设备
                os_log(OS_LOG_DEFAULT, "%s: Found BULK OUT endpoint: 0x%02x\n",
                       RNDIS_LOG, epAddr);

                ret = fDataInterface->CopyPipe(epAddr, &fOutPipe);
                if (ret != kIOReturnSuccess) {
                    os_log(OS_LOG_DEFAULT, "%s: CopyPipe(OUT) failed: 0x%x\n", RNDIS_LOG, ret);
                    return ret;
                }
            }
        }
        epIndex++;
    }

    // 验证两个管道都已获取
    if (!fInPipe || !fOutPipe) {
        os_log(OS_LOG_DEFAULT, "%s: Missing pipes — IN=%p OUT=%p\n",
                     RNDIS_LOG, fInPipe, fOutPipe);
        return kIOReturnNoResources;
    }

    os_log(OS_LOG_DEFAULT, "%s: Pipes opened successfully\n", RNDIS_LOG);
    return kIOReturnSuccess;
}

// ============================================================
// rndisCommand — 发送 RNDIS 命令并等待响应
// ============================================================
/*
 * 核心控制通道通信函数。
 *
 * 流程：
 *   1. 构造 DeviceRequest（USB 控制传输）
 *      - bmRequestType: OUT | Class | Interface
 *      - bRequest: USB_CDC_SEND_ENCAPSULATED_COMMAND
 *   2. 通过控制端点发送命令
 *   3. 轮询读取响应（最多 10 次重试，每次间隔 20ms）
 *      - bRequest: USB_CDC_GET_ENCAPSULATED_RESPONSE
 *   4. 校验响应: msg_type 匹配 | request_id 匹配 | status == SUCCESS
 */
kern_return_t RNDISUSBDevice::rndisCommand(
    uint32_t    msg_type,
    const void *send_buf,
    uint32_t    send_len,
    void       *recv_buf,
    uint32_t   *recv_len)
{
    kern_return_t ret;

    if (!fCommInterface) {
        os_log(OS_LOG_DEFAULT, "%s: rndisCommand: no comm interface\n", RNDIS_LOG);
        return kIOReturnNoDevice;
    }

    // 步骤 1: 发送命令
    ret = sendControlCommand(send_buf, send_len);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: sendControlCommand failed: 0x%x\n", RNDIS_LOG, ret);
        return ret;
    }

    // 步骤 2: 轮询读取响应（最多 10 次重试）
    uint32_t response_len = *recv_len;
    for (int i = 0; i < 10; i++) {
        IOSleep(20);  // 等待设备处理命令

        response_len = *recv_len;
        ret = recvControlResponse(recv_buf, &response_len);
        if (ret != kIOReturnSuccess) {
            continue;  // 继续重试
        }

        // 验证响应
        if (response_len < sizeof(rndis_msg_hdr)) {
            os_log(OS_LOG_DEFAULT, "%s: response too short: %u\n", RNDIS_LOG, response_len);
            continue;
        }

        rndis_msg_hdr *resp = (rndis_msg_hdr *)recv_buf;

        // 校验 msg_type（响应应为请求类型 | RNDIS_MSG_COMPLETION）
        uint32_t expected_type = msg_type | RNDIS_MSG_COMPLETION;
        if (resp->msg_type != expected_type) {
            os_log(OS_LOG_DEFAULT, "%s: unexpected msg_type: expected 0x%x, got 0x%x\n",
                         RNDIS_LOG, expected_type, resp->msg_type);
            continue;
        }

        // 校验 request_id
        rndis_msg_hdr *req = (rndis_msg_hdr *)send_buf;
        if (resp->request_id != req->request_id) {
            os_log(OS_LOG_DEFAULT, "%s: request_id mismatch: expected %u, got %u\n",
                         RNDIS_LOG, req->request_id, resp->request_id);
            continue;
        }

        // 校验状态码
        if (resp->status != RNDIS_STATUS_SUCCESS) {
            os_log(OS_LOG_DEFAULT, "%s: command failed, status=0x%x\n",
                         RNDIS_LOG, resp->status);
            return kIOReturnIOError;
        }

        // 校验 msg_len 与实际传输长度一致
        if (resp->msg_len != response_len) {
            os_log(OS_LOG_DEFAULT, "%s: msg_len mismatch: header=%u, actual=%u\n",
                   RNDIS_LOG, resp->msg_len, response_len);
        }

        *recv_len = response_len;
        return kIOReturnSuccess;
    }

    os_log(OS_LOG_DEFAULT, "%s: rndisCommand timeout after 10 retries\n", RNDIS_LOG);
    return kIOReturnTimeout;
}

// ============================================================
// sendControlCommand — 通过 USB 控制端点发送命令
// ============================================================
kern_return_t RNDISUSBDevice::sendControlCommand(const void *buf, uint32_t len)
{
    // 构造 DeviceRequest: OUT | Class | Interface

    uint8_t  bmRequestType = 0x21;  // Host→Device, Class, Interface
    uint8_t  bRequest      = USB_CDC_SEND_ENCAPSULATED_COMMAND;
    uint16_t wValue        = 0;
    uint16_t wIndex        = fCommIfNum;
    uint16_t wLength       = len;

    // 创建发送缓冲区
    IOBufferMemoryDescriptor *cmdBuf = nullptr;
    IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOut, len, 0, &cmdBuf);
    if (!cmdBuf) return kIOReturnNoMemory;

    IOAddressSegment seg;
    cmdBuf->GetAddressRange(&seg);
    memcpy(reinterpret_cast<void *>(seg.address), buf, len);
    cmdBuf->SetLength(len);

    uint16_t bytesTransferred = 0;
    kern_return_t ret = fCommInterface->DeviceRequest(
        bmRequestType, bRequest, wValue, wIndex, wLength,
        cmdBuf,   // IOMemoryDescriptor 数据缓冲区
        &bytesTransferred,
        5000);  // 5 秒超时
    
    cmdBuf->release();

    return ret;
}

// ============================================================
// recvControlResponse — 通过 USB 控制端点接收响应
// ============================================================
kern_return_t RNDISUSBDevice::recvControlResponse(void *buf, uint32_t *len)
{
    // 构造 DeviceRequest: IN | Class | Interface (DriverKit 25.5)
    // 创建接收缓冲区
    IOBufferMemoryDescriptor *recvBuf = nullptr;
    IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionIn, *len, 0, &recvBuf);
    if (!recvBuf) return kIOReturnNoMemory;

    uint16_t bytesTransferred = 0;
    kern_return_t ret = fCommInterface->DeviceRequest(
        0xA1, USB_CDC_GET_ENCAPSULATED_RESPONSE, 0, fCommIfNum, *len,
        recvBuf,
        &bytesTransferred,
        5000);
    
    if (ret == kIOReturnSuccess && bytesTransferred > 0) {
        IOAddressSegment seg;
        recvBuf->GetAddressRange(&seg);
        memcpy(buf, (void *)seg.address, bytesTransferred);
        *len = bytesTransferred;
    }
    recvBuf->release();

    return ret;
}

// ============================================================
// rndisInit — RNDIS 初始化序列
// ============================================================
/*
 * 发送 RNDIS_MSG_INIT 并接收 RNDIS_MSG_INIT_C。
 * 关键：从响应中获取 max_transfer_size，这是设备允许的最大单次传输字节数。
 */
kern_return_t RNDISUSBDevice::rndisInit(void)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "%s: rndisInit()\n", RNDIS_LOG);

    // 构造初始化消息
    rndis_init init_msg;
    memset(&init_msg, 0, sizeof(init_msg));
    init_msg.msg_type         = RNDIS_MSG_INIT;
    init_msg.msg_len          = sizeof(rndis_init);
    init_msg.request_id       = ++fRequestID;
    init_msg.major_version    = 1;
    init_msg.minor_version    = 0;
    init_msg.max_transfer_size = IN_BUF_SIZE;

    // 发送并接收响应
    uint8_t  recv_buf[RNDIS_CMD_BUF_SZ];
    uint32_t recv_len = sizeof(recv_buf);
    memset(recv_buf, 0, recv_len);

    ret = rndisCommand(RNDIS_MSG_INIT, &init_msg, sizeof(init_msg),
                        recv_buf, &recv_len);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: rndisCommand(INIT) failed: 0x%x\n", RNDIS_LOG, ret);
        return ret;
    }

    // 解析响应
    rndis_init_c *init_c = (rndis_init_c *)recv_buf;
    os_log(OS_LOG_DEFAULT, "%s: INIT_C: major=%u minor=%u medium=%u max_transfer=%u max_pkts=%u\n",
           RNDIS_LOG,
           init_c->major_version,
           init_c->minor_version,
           init_c->medium,
           init_c->max_transfer_size,
           init_c->max_packets_per_transfer);

    // 存储设备允许的最大传输单元
    maxTransferSize = init_c->max_transfer_size;
    if (maxTransferSize == 0 || maxTransferSize > IN_BUF_SIZE) {
        maxTransferSize = IN_BUF_SIZE;
    }

    os_log(OS_LOG_DEFAULT, "%s: Using maxTransferSize=%u\n", RNDIS_LOG, maxTransferSize);

    return kIOReturnSuccess;
}

// ============================================================
// rndisQuery — 查询指定 OID
// ============================================================
kern_return_t RNDISUSBDevice::rndisQuery(uint32_t oid,
                                          void    *info_buf,
                                          uint32_t *info_len)
{
    kern_return_t ret;

    // 构造查询消息（消息头 + 信息缓冲区在命令缓冲区中拼接）
    uint8_t cmd_buf[RNDIS_CMD_BUF_SZ];
    memset(cmd_buf, 0, sizeof(cmd_buf));

    rndis_query *query = (rndis_query *)cmd_buf;
    query->msg_type     = RNDIS_MSG_QUERY;
    query->msg_len      = sizeof(rndis_query);
    query->request_id   = ++fRequestID;
    query->oid          = oid;
    query->info_buflen  = 0;
    query->info_bufoff  = 0;
    query->dev_vc_handle = 0;

    // 发送并接收响应
    uint8_t  recv_buf[RNDIS_CMD_BUF_SZ];
    uint32_t recv_len = sizeof(recv_buf);
    memset(recv_buf, 0, recv_len);

    ret = rndisCommand(RNDIS_MSG_QUERY, cmd_buf, sizeof(rndis_query),
                        recv_buf, &recv_len);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: rndisQuery(0x%x) failed: 0x%x\n", RNDIS_LOG, oid, ret);
        return ret;
    }

    // 解析响应中的信息缓冲区
    rndis_query_c *query_c = (rndis_query_c *)recv_buf;
    if (query_c->info_buflen > 0 && query_c->info_bufoff >= sizeof(rndis_query_c)) {
        uint32_t copy_len = query_c->info_buflen;
        if (copy_len > *info_len) {
            copy_len = *info_len;
        }
        memcpy(info_buf, recv_buf + query_c->info_bufoff, copy_len);
        *info_len = copy_len;
    } else {
        *info_len = 0;
    }

    return kIOReturnSuccess;
}

// ============================================================
// rndisSetPacketFilter — 设置包过滤器
// ============================================================
kern_return_t RNDISUSBDevice::rndisSetPacketFilter(uint32_t filter)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "%s: rndisSetPacketFilter(0x%x)\n", RNDIS_LOG, filter);

    // 构造设置消息（消息头 + 4 字节的过滤器值）
    uint8_t cmd_buf[RNDIS_CMD_BUF_SZ];
    memset(cmd_buf, 0, sizeof(cmd_buf));

    rndis_set *set = (rndis_set *)cmd_buf;
    set->msg_type      = RNDIS_MSG_SET;
    set->msg_len       = sizeof(rndis_set) + sizeof(uint32_t);
    set->request_id    = ++fRequestID;
    set->oid           = OID_GEN_CURRENT_PACKET_FILTER;
    set->info_buflen   = sizeof(uint32_t);
    set->info_bufoff   = sizeof(rndis_set);
    set->dev_vc_handle = 0;

    // 将过滤器值写入消息后的信息缓冲区
    uint32_t *filter_val = (uint32_t *)(cmd_buf + sizeof(rndis_set));
    *filter_val = filter;

    // 发送并接收响应
    uint8_t  recv_buf[RNDIS_CMD_BUF_SZ];
    uint32_t recv_len = sizeof(recv_buf);
    memset(recv_buf, 0, recv_len);

    ret = rndisCommand(RNDIS_MSG_SET, cmd_buf, set->msg_len,
                        recv_buf, &recv_len);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: rndisSetPacketFilter failed: 0x%x\n", RNDIS_LOG, ret);
    }

    return ret;
}

// ============================================================
// sendPacket — 通过 BULK OUT 发送以太网帧
// ============================================================
/*
 * 组装 RNDIS 数据头 + 以太网帧，通过 USB BULK OUT 异步发送。
 *
 * 格式: [rndis_data_hdr (44 bytes)] [Ethernet Frame]
 *
 * 缓冲区管理：使用环形发送缓冲区（fOutBufs），发送完成后
 * sendComplete 回调中归还，确保新数据可立即使用。
 */
kern_return_t RNDISUSBDevice::sendPacket(const void *data, uint32_t data_len)
{
    kern_return_t ret;

    if (!fReadyToTransfer || !fOutPipe) {
        return kIOReturnNotReady;
    }

    // 获取当前可用发送缓冲区
    IOBufferMemoryDescriptor *outBuf = fOutBufs[fOutBufIndex];
    fOutBufIndex = (fOutBufIndex + 1) % N_OUT_BUFS;  // 切换到下一个

    // 清空缓冲区 (DriverKit 25.5: GetAddressRange takes IOAddressSegment*)
    IOAddressSegment addrSeg;
    outBuf->GetAddressRange(&addrSeg);
    uint64_t bufAddr = addrSeg.address;
    uint64_t bufLen  = addrSeg.length;

    // 构造 RNDIS 数据包头（44 字节）
    rndis_data_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_type           = RNDIS_MSG_PACKET;
    hdr.msg_len            = sizeof(rndis_data_hdr) + data_len;
    hdr.data_offset        = sizeof(rndis_data_hdr) - 8;  // 相对偏移
    hdr.data_len           = data_len;
    hdr.oob_data_offset    = 0;
    hdr.oob_data_len       = 0;
    hdr.num_oob            = 0;
    hdr.packet_data_offset = sizeof(rndis_data_hdr) - 8;
    hdr.packet_data_len    = data_len;
    hdr.vc_handle          = 0;
    hdr.reserved           = 0;

    // 将 RNDIS 数据头和以太网帧写入缓冲区
    // 注意：DriverKit 中 IOBufferMemoryDescriptor 的写入方式
    uint8_t *buf_ptr = (uint8_t *)bufAddr;
    memcpy(buf_ptr, &hdr, sizeof(hdr));
    memcpy(buf_ptr + sizeof(hdr), data, data_len);

    uint32_t total_len = sizeof(rndis_data_hdr) + data_len;

    // 提交异步 BULK OUT 传输
    ret = fOutPipe->AsyncIO(
        outBuf,
        total_len,
        nullptr,   // completion OSAction (needs CreateActionAsyncIO)
        0);        // completionTimeoutMs

    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: AsyncIO(OUT) failed: 0x%x\n", RNDIS_LOG, ret);
    }

    return ret;
}

// ============================================================
// sendComplete — BULK OUT 发送完成回调
// ============================================================
void RNDISUSBDevice::sendComplete(void *refcon,
                                   kern_return_t status,
                                   IOBufferMemoryDescriptor *buffer,
                                   uint64_t actualByteCount)
{
    RNDISUSBDevice *self = (RNDISUSBDevice *)refcon;

    if (status != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: sendComplete failed: 0x%x\n", RNDIS_LOG, status);
        self->fDataDead = true;
    }
    // 缓冲区在 sendPacket() 中自动循环重用，无需额外处理
}

// ============================================================
// startAsyncReceive — 启动异步 BULK IN 接收
// ============================================================
/*
 * 提交一个异步 BULK IN 读取请求。每次接收完成后，recvComplete
 * 会解析数据并自动重新提交下一次读取，形成持续的接收循环。
 */
kern_return_t RNDISUSBDevice::startAsyncReceive(void)
{
    kern_return_t ret;

    if (!fReadyToTransfer || !fInPipe) {
        return kIOReturnNotReady;
    }

    ret = fInPipe->AsyncIO(
        fInBuf,
        IN_BUF_SIZE,
        nullptr,   // completion OSAction (needs CreateActionAsyncIO)
        0);        // completionTimeoutMs

    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: AsyncIO(IN) failed: 0x%x\n", RNDIS_LOG, ret);
    }

    return ret;
}

// ============================================================
// recvComplete — BULK IN 接收完成回调
// ============================================================
/*
 * 处理从 USB BULK IN 管道接收到的数据。
 *
 * 解析 RNDIS 数据头：
 *   - 校验 msg_type == RNDIS_MSG_PACKET
 *   - 解析 data_offset / data_len 提取以太网帧
 *   - 按 msg_len 推进，支持单次传输中的多包聚合
 *   - 将提取的以太网帧投递给 RNDISEthernetController
 *
 * 解析完成后自动重新提交下一次异步读取。
 */
void RNDISUSBDevice::recvComplete(void *refcon,
                                   kern_return_t status,
                                   IOBufferMemoryDescriptor *buffer,
                                   uint64_t actualByteCount)
{
    RNDISUSBDevice *self = (RNDISUSBDevice *)refcon;

    if (status != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "%s: recvComplete failed: 0x%x\n", RNDIS_LOG, status);
        self->fDataDead = true;
        return;
    }

    if (!self->fReadyToTransfer) {
        return;
    }

    // 获取缓冲区数据
    IOAddressSegment addrSeg;
    buffer->GetAddressRange(&addrSeg);
    uint64_t bufAddr = addrSeg.address;
    uint64_t bufLen  = addrSeg.length;

    uint8_t  *ptr    = (uint8_t *)bufAddr;
    uint64_t  offset = 0;

    // 支持单次传输中的多包聚合
    // 循环解析每个 RNDIS 数据包，直到缓冲区耗尽
    while (offset + sizeof(rndis_data_hdr) <= actualByteCount) {
        rndis_data_hdr *hdr = (rndis_data_hdr *)(ptr + offset);

        // 校验消息类型
        if (hdr->msg_type != RNDIS_MSG_PACKET) {
            os_log(OS_LOG_DEFAULT, "%s: unexpected msg_type in data: 0x%x\n",
                         RNDIS_LOG, hdr->msg_type);
            break;
        }

        // 校验消息长度
        if (hdr->msg_len < sizeof(rndis_data_hdr) ||
            offset + hdr->msg_len > actualByteCount) {
            os_log(OS_LOG_DEFAULT, "%s: invalid msg_len: %u (offset=%llu, total=%llu)\n",
                         RNDIS_LOG, hdr->msg_len, offset, actualByteCount);
            break;
        }

        // 提取以太网帧
        uint32_t data_offset = hdr->data_offset + 8;  // offset 基址修正
        uint32_t data_len    = hdr->data_len;

        if (data_len > 0 &&
            data_offset + data_len <= hdr->msg_len &&
            offset + data_offset + data_len <= actualByteCount) {

            // 将提取的以太网帧投递给网络控制器
            if (self->ethernetController) {
                self->ethernetController->receivePacket(
                    ptr + offset + data_offset, data_len);
            }
        }

        // 推进到下一个包
        offset += hdr->msg_len;
    }

    // 重新提交异步读取
    if (self->fReadyToTransfer && !self->fDataDead) {
        self->startAsyncReceive();
    }
}
