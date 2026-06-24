/*
 * main.cpp — DriverKit dext 入口
 *
 * RNDISDriver dext 的启动/停止入口点。
 *
 * 职责：
 *   - 注册 RNDISEthernetController 和 RNDISUSBDevice 服务类
 *   - 在 dext 加载时执行初始化
 *   - 在 dext 卸载时执行清理
 */

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <os/log.h>

#include "RNDISEthernetController.h"
#include "RNDISUSBDevice.h"

#define RNDIS_LOG "com.yourcompany.RNDISDriver"

/*
 * dext 启动入口
 *
 * 当系统加载 dext 时调用。此时应注册所有服务类，
 * 使得 IOKit 匹配系统能够自动发现和启动它们。
 */
extern "C" kern_return_t dext_start(kmod_info_t *ki, void *data)
{
    kern_return_t ret;

    os_log(OS_LOG_DEFAULT, "%s: dext_start() — registering services\n", RNDIS_LOG);

    // 注册 RNDISEthernetController 元类
    // 这使得系统可以通过 Info.plist 的 IOKitPersonalities 匹配
    // 到此类并实例化
    ret = RNDISEthernetController::RegisterService();
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "%s: Failed to register RNDISEthernetController: 0x%x\n",
                     RNDIS_LOG, ret);
        return ret;
    }
    os_log(OS_LOG_DEFAULT, "%s: RNDISEthernetController registered\n", RNDIS_LOG);

    // 注册 RNDISUSBDevice 元类
    // 注意：实际的 USB 设备匹配由 Info.plist 中的 IOKitPersonalities
    // 决定。RNDISUSBDevice 作为 IOService 被 USB 子系统发现，
    // 然后作为 provider 传递给 RNDISEthernetController。
    ret = RNDISUSBDevice::RegisterService();
    if (ret != kIOReturnSuccess) {
        os_log_error(OS_LOG_DEFAULT, "%s: Failed to register RNDISUSBDevice: 0x%x\n",
                     RNDIS_LOG, ret);
        return ret;
    }
    os_log(OS_LOG_DEFAULT, "%s: RNDISUSBDevice registered\n", RNDIS_LOG);

    os_log(OS_LOG_DEFAULT, "%s: dext_start() completed\n", RNDIS_LOG);

    return kIOReturnSuccess;
}

/*
 * dext 停止入口
 *
 * 当系统卸载 dext 时调用。
 * DriverKit 框架会自动处理服务注销和资源清理，
 * 因此此函数通常只需返回成功即可。
 */
extern "C" kern_return_t dext_stop(kmod_info_t *ki, void *data)
{
    os_log(OS_LOG_DEFAULT, "%s: dext_stop()\n", RNDIS_LOG);

    // 无需显式注销服务 — 系统自动处理

    return kIOReturnSuccess;
}

// ============================================================
// kmod_info 数据结构
// ============================================================
/*
 * DriverKit 框架要求的模块信息结构。
 * dext_start / dext_stop 函数指针对应上述实现。
 */
extern "C" {
    kmod_info_t kmod_info = {
        .next       = nullptr,
        .name       = "com.yourcompany.RNDISDriver",
        .version    = 1,
        .start      = dext_start,
        .stop       = dext_stop,
    };
}
