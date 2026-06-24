/*
 * main.cpp — DriverKit dext 入口（DriverKit 25.5）
 *
 * RNDISDriver dext 的启动/停止入口点。
 *
 * DriverKit 25.5 变更：
 *   - kmod_info_t / kmod_start / kmod_stop 已移除
 *   - 使用 __attribute__((constructor/destructor)) 替代
 *   - 服务类通过 IIG (.iig) 系统自动注册，无需手动 RegisterService()
 */

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <os/log.h>

#define RNDIS_LOG "com.yourcompany.RNDISDriver"

/*
 * dext 加载时的初始化
 *
 * 系统加载 dext 后自动调用。DriverKit 25.5 中，
 * 类注册由 IIG 系统根据 .iig 文件自动完成。
 */
__attribute__((constructor))
static void dextInit(void)
{
    os_log(OS_LOG_DEFAULT, "%s: dext loaded\n", RNDIS_LOG);
}

/*
 * dext 卸载时的清理
 *
 * 系统卸载 dext 前自动调用。框架自动处理服务注销和资源清理。
 */
__attribute__((destructor))
static void dextFini(void)
{
    os_log(OS_LOG_DEFAULT, "%s: dext unloaded\n", RNDIS_LOG);
}
