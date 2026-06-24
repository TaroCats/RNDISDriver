/*
 * main.swift — RNDIS App 宿主程序入口
 *
 * 职责：
 *   - 请求系统扩展 (dext) 激活
 *   - 显示驱动安装状态
 *   - 提供简单的状态 UI
 *
 * 注意：系统扩展激活需要用户手动在"系统设置 → 隐私与安全性"中批准。
 */

import Foundation
import SystemExtensions
import AppKit

/// 系统扩展请求管理器
class DextLaunchManager: NSObject, OSSystemExtensionRequestDelegate {

    // ============================================================
    // 系统扩展激活
    // ============================================================

    /// 请求激活 RNDISDriver 系统扩展
    func activateExtension() {
        let extensionIdentifier = "com.yourcompany.RNDISDriver"

        print("[RNDISApp] Requesting activation of \(extensionIdentifier)...")

        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: extensionIdentifier,
            queue: DispatchQueue.main
        )
        request.delegate = self

        OSSystemExtensionManager.shared.submitRequest(request)
    }

    /// 请求停用系统扩展
    func deactivateExtension() {
        let extensionIdentifier = "com.yourcompany.RNDISDriver"

        print("[RNDISApp] Requesting deactivation of \(extensionIdentifier)...")

        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: extensionIdentifier,
            queue: DispatchQueue.main
        )
        request.delegate = self

        OSSystemExtensionManager.shared.submitRequest(request)
    }

    // ============================================================
    // OSSystemExtensionRequestDelegate
    // ============================================================

    func request(
        _ request: OSSystemExtensionRequest,
        actionForReplacingExtension existing: OSSystemExtensionProperties,
        withExtension ext: OSSystemExtensionProperties
    ) -> OSSystemExtensionRequest.ReplacementAction {
        print("[RNDISApp] System extension needs replacement: \(existing.bundleShortVersion) -> \(ext.bundleShortVersion)")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        print("[RNDISApp] ⚠️  System extension needs user approval.")
        print("[RNDISApp] Please go to: System Settings → Privacy & Security")
        print("[RNDISApp] Scroll down to 'Security' and click 'Allow' for RNDISDriver.")
    }

    func request(
        _ request: OSSystemExtensionRequest,
        didFinishWithResult result: OSSystemExtensionRequest.Result
    ) {
        print("[RNDISApp] System extension activation result: \(result.rawValue)")
        if result == .completed {
            print("[RNDISApp] ✅ RNDISDriver installed and ready.")
        }
    }

    func request(
        _ request: OSSystemExtensionRequest,
        didFailWithError error: Error
    ) {
        print("[RNDISApp] ❌ System extension activation failed: \(error.localizedDescription)")
    }
}

// ============================================================
// App 入口
// ============================================================

class AppDelegate: NSObject, NSApplicationDelegate {
    var manager: DextLaunchManager!

    func applicationDidFinishLaunching(_ notification: Notification) {
        manager = DextLaunchManager()

        // 自动请求激活系统扩展
        manager.activateExtension()
    }

    func applicationWillTerminate(_ notification: Notification) {
        print("[RNDISApp] Terminating.")
    }
}

// 启动 App
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)  // 隐藏 Dock 图标
app.run()
