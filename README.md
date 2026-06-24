# RNDISDriver — macOS Network Extension (DriverKit)

基于 [HoRNDIS](https://github.com/jwise/HoRNDIS) 项目，将 RNDIS 协议驱动从内核扩展 (kext) 迁移到现代 **DriverKit Network Extension (dext)**，以兼容 macOS 11+ 的安全模型。

## 项目背景

RNDIS (Remote NDIS) 是微软定义的一种通过 USB 共享网络的协议。大多数 Android 手机和嵌入式 Linux 设备使用 RNDIS 提供 USB 网络共享 (USB Tethering) 功能。

自 macOS 11 起，Apple 逐步弃用内核扩展（kext），要求驱动以 DriverKit 扩展（dext）的形式运行在用户空间。本项目基于 HoRNDIS 的成熟 RNDIS 协议实现，将其迁移到 DriverKit 框架。

## 项目结构

```
RNDISDriver/
├── RNDISDriver/                  # dext 驱动 target
│   ├── main.cpp                  # dext 入口，注册服务类
│   ├── RNDISProtocol.h           # RNDIS 协议定义（数据结构/消息枚举/OID）
│   ├── RNDISUSBDevice.h          # USB 设备管理类头文件
│   ├── RNDISUSBDevice.cpp        # USB 设备管理实现（Probe/Start/命令通信/数据收发）
│   ├── RNDISEthernetController.h # 以太网控制器头文件
│   ├── RNDISEthernetController.cpp # 以太网控制器实现（网络接口/包收发）
│   └── Info.plist                # dext 配置（IOKitPersonalities 设备匹配）
├── RNDISApp/                     # 宿主 App target
│   ├── main.swift                # App 入口（系统扩展激活）
│   └── Info.plist                # App 配置
├── RNDISDriver.entitlements      # dext 签名授权
└── README.md                     # 项目说明
```

## 支持设备

通过 4 个 IOKitPersonalities 匹配规则，覆盖以下 RNDIS 设备类型：

| Personality | 匹配条件 | 典型设备 |
|------------|---------|---------|
| `RNDISControlStockAndroid` | Class=224/Sub=1/Proto=3 | Google Pixel, Samsung, 小米等 |
| `RNDISControlMiscDeviceRoE` | Class=239/Sub=4/Proto=1 | Nokia 7 Plus, Sony Xperia XZ |
| `RNDISControlLinuxGadget` | Class=2/Sub=2/Proto=255 | BeagleBoard, PlutoSDR |
| `WirelessControllerDevice` | Class=224 (设备级) | Samsung Galaxy S7 Edge |

## RNDIS 协议核心流程

```
1. init() — 发送 RNDIS_MSG_INIT
   ├─ major_version=1, minor_version=0
   └─ 接收 RNDIS_MSG_INIT_C → 获取 max_transfer_size

2. rndisQuery(OID_802_3_PERMANENT_ADDRESS) — 获取 MAC 地址

3. rndisSetPacketFilter(RNDIS_DEFAULT_FILTER) — 设置包过滤器

4. 正常运行 — 收发以太网帧
```

## 构建要求

### 环境
- macOS 13.0+ (Ventura)
- Xcode 15.0+
- Apple Developer Account（用于代码签名）

### 前置条件：创建 Xcode 项目

项目当前仅包含源码文件，**不包含 `.xcodeproj` 工程文件**。在本地或 CI 中构建前，需要先在 Xcode 中创建项目并配置 scheme：

1. 打开 Xcode → **File → New → Project**
2. 选择 **macOS → DriverKit → Network Extension (dext)** 模板，项目名 `RNDISDriver`
3. 在项目中添加 **App Target**：`RNDISApp`（macOS → App，包类型选 System Extension）
4. 将本仓库中的 `.cpp` / `.h` / `.swift` / `.plist` / `.entitlements` 文件拖入对应 target：
   - `RNDISDriver/` 目录下的文件 → `RNDISDriver` (dext) target
   - `RNDISApp/` 目录下的文件 → `RNDISApp` target
5. 在 Xcode 中配置 **Signing & Capabilities**（App 和 dext 两个 target 分别配置）
6. 确认 scheme 构建顺序：dext 先于 App

配置完成后，生成的 `.xcodeproj` 文件建议一并提交到仓库，以便 CI 直接使用。

### 本地构建步骤

1. 在 Xcode 中打开项目
2. 配置 Bundle Identifier（替换 `com.yourcompany` 为你的 Team ID）
3. 配置 entitlements 签名：

   ```bash
   # 需要在 Apple Developer Portal 中申请以下 entitlements:
   # - DriverKit (com.apple.developer.driverkit)
   # - DriverKit USB Transport (com.apple.developer.driverkit.transport.usb)
   # - DriverKit Networking (com.apple.developer.driverkit.transport.networking)
   # - DriverKit UserClient Access (com.apple.developer.driverkit.userclient-access)
   ```

4. Build (Cmd+B)

### CI 构建 (GitHub Actions)

项目包含 `.github/workflows/build.yml`，在 push / pull_request 时自动触发：

| Job | Runner | 说明 |
|-----|--------|------|
| `build` | `macos-14` (Apple Silicon) | Debug + Release 双配置无签名构建，验证编译通过 |
| `lint` | `macos-14` | 校验所有 `.plist` 和 `.entitlements` 格式合法性 |
| `release` | `macos-14` | 仅 `workflow_dispatch` 手动触发，签名 + 公证（需配置 Secrets） |

**CI 前置条件**：
- `.xcodeproj` 已按上述步骤创建并提交到仓库
- Scheme 名称与 workflow 中的 `-scheme RNDISDriver` 一致

**Release Job 所需 Secrets**（仅在需要签名公证时配置）：

| Secret | 说明 |
|--------|------|
| `APPLE_DEVELOPER_CERTIFICATE` | Developer ID Application 证书（Base64 编码的 .p12） |
| `APPLE_DEVELOPER_CERTIFICATE_PASSWORD` | .p12 证书密码 |
| `APPLE_NOTARIZATION_USERNAME` | Apple ID（用于公证） |
| `APPLE_NOTARIZATION_PASSWORD` | App 专用密码 |
| `APPLE_NOTARIZATION_TEAM_ID` | Apple Developer Team ID |

### 产物
- `RNDISDriver.dext` — DriverKit 系统扩展
- `RNDISApp.app` — 宿主应用

## 安装步骤

1. 执行 `RNDISApp.app`
2. App 会自动请求系统扩展激活
3. 前往 **系统设置 → 隐私与安全性**，滚动到底部点击"允许"
4. 插入 Android 手机并开启 USB 网络共享
5. 系统设置 → 网络 中会出现新的以太网接口

## 签名要求

- Debug 构建：使用 Development Signing
- Release 构建：需要 Notarization

两个 target 都需要签名：
- `RNDISApp.app`：标准的 macOS App 签名
- `RNDISDriver.dext`：DriverKit 扩展签名（需要上述 entitlements）

## 从 HoRNDIS (kext) 到本项目的迁移对照

| kext 组件 | Network Extension 对应 |
|----------|----------------------|
| `IOEthernetController` | `IOUserEthernetController` |
| `IOEthernetInterface` | `IOUserEthernetInterface` |
| `IOUSBHostDevice/Interface` | `IOUSBHostDevice/Interface` (DriverKit) |
| `IOUSBHostPipe::io()` | `IOUSBHostPipe::AsyncIO()` |
| `IOBufferMemoryDescriptor` | `IOBufferMemoryDescriptor::Create()` |
| `IOGatedOutputQueue` | 使用框架内置队列管理 |
| `IOLog` | `os_log` |
| `IOSleep` | `IOSleep` (可用) |
| RNDIS 协议逻辑 | **完全可复用** |

## 技术要点

1. **内存管理**：所有缓冲区使用 `IOBufferMemoryDescriptor::Create()` 分配，遵循 DriverKit 受限内存模型
2. **异步 IO**：USB BULK 传输使用 `AsyncIO()` 模式，完成回调自动串行化
3. **设备发现**：`Probe()` 阶段验证 USB 接口的 class/subclass/protocol 三元组，支持三种 RNDIS 设备变体
4. **多包聚合**：接收路径支持单次 USB 传输中的多个 RNDIS 数据包解析
5. **协议兼容**：RNDIS 协议数据结构完全沿用 [MS-RNDIS] 规范，与 HoRNDIS 保持一致

## 参考来源

- [HoRNDIS](https://github.com/jwise/HoRNDIS) — 原始 kext 实现
- [MS-RNDIS] — Microsoft RNDIS 协议规范
- [MSDN-RNDISUSB] — RNDIS USB 实现指南
- [Apple DriverKit Documentation](https://developer.apple.com/documentation/driverkit)
- [Apple USBDriverKit](https://developer.apple.com/documentation/usbdriverkit)
- [Apple NetworkDriverKit](https://developer.apple.com/documentation/networkdriverkit)

## 许可证

本项目基于 HoRNDIS (GPL v2) 重新实现。如分发二进制版本，请遵守 GPL v2 条款。

## 已知限制

1. 当前实现为单次 BULK IN 接收，未做双缓冲优化
2. 多播过滤为全集设置模式，未实现精细的多播地址列表
3. KeepAlive 机制未实现（Android 不依赖此机制）
4. 不支持 RNDIS 数据包的 OOB (Out-of-Band) 数据
