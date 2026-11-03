> **yoni13 fork.** This is a fork of [lonzzi/UniXNU-houji](https://github.com/lonzzi/UniXNU-houji) (upstream, forked at
> `99beb10`). It adds Wi-Fi, battery charging, buttons, display power and brightness, idle-power and interrupt work, GPU
> coherence in its own kernel, and modem/GPS and camera bring-up. English documentation: [docs/fork.md](docs/fork.md)
> (sources, differences, building) and [docs/peripherals.md](docs/peripherals.md) (per-peripheral status). The rest of this
> README is upstream's.

<div align="center">

# UniXNU · houji

**小米 14 的原生 iOS 移植**

Snapdragon 8 Gen 3 · Adreno 750 · Darwin 22

[功能状态](#功能状态) · [性能修复](docs/performance.md) · [待完成项](docs/roadmap.md) · [开发指南](docs/development.md) · [代码结构](docs/architecture.md) · [问题反馈](https://github.com/lonzzi/UniXNU-houji/issues)

</div>

基于 [UniXNU](https://github.com/UniXNU/UniXNU) 生态，为小米 14（`houji`）适配 UEFI、XNU 内核、硬件驱动及 iOS 用户空间。目前已经能够进入 iOS 桌面，使用原生分辨率显示、触控、八核 CPU 和实验性的 Adreno 图形加速。

项目仍处于开发阶段。GPU 性能、颜色与部分应用的兼容性尚有问题，暂不适合作为日常系统，也没有面向普通用户的一键刷机包。

## 目标设备

| 项目 | 配置 |
| --- | --- |
| 机型 | Xiaomi 14 / 小米 14，`houji` |
| 已测试型号 | 23127PN0CC |
| SoC | Qualcomm SM8650，Snapdragon 8 Gen 3 |
| GPU | Adreno 750 |
| 显示 | 1200 × 2670，原生像素输出 |
| 测试系统 | iOS 16.7.16，20H392，iPhone X / `d22ap` 用户空间 |
| 内核 | Darwin 22，自行编译的 XNU 8796.141.3 |
| 引导 | Mu-Silicium UEFI → DarwinBoot → XNU |

## 功能状态

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| UEFI 与系统启动 | 可用 | 已实机进入锁屏、桌面和设置 |
| CPU 多核 | 可用 | 八核启动，适配大小核分组与调度 |
| CPU / GPU 调频 | 实验性 | 使用工厂频率表、负载与触控提示，已有 TSENS 结温限制；持续性能和完整热保护待完善 |
| 显示与触控 | 可用 | 原生分辨率、全屏触控、返回与最近任务手势 |
| 系统键盘 | 可用 | 设置、主屏搜索、App 资源库的宽度适配 |
| UFS 系统卷 | 可用 | 当前开发配置使用 32 GiB HFSX 卷 |
| Adreno 图形加速 | 部分可用 | 已有真实命令提交、GPU 写回和桌面渲染；掉帧、紫色阴影、文字异常仍待修复 |
| 墙纸与照片 | 不完整 | 墙纸预览不稳定；照片应用存在图库初始化失败导致的黑屏，兼容修复待实机验证 |
| 日志与恢复 | 可用 | [采集工具](docs/log-capture.md)保存原始分区、校验检查点并比较本轮应用崩溃报告；早期锁死仍可能漏记 |
| Wi-Fi / 蓝牙 / 蜂窝 | 待适配 | 尚未完成可用性验证 |
| 亮度 | 调节初步可用 | 已实机验证亮度响应；完整亮度范围与熄屏/唤醒仍待验证 |
| 电量与 USB 充电状态 | 基础功能可用 | 经原厂 ADSP/PMIC GLINK 读取真实电量，已验证插拔状态切换；充电控制、电流和电池温度保护待完善 |
| 声音 | 待适配 | ADSP 通信已接通；GPR、AudioReach、WCD939x 与双 CS35L41 音频路由尚未完成 |
| Safari 等应用 | 待验证 | 依赖网络和图形兼容性继续完善 |
| 系统应用 | 已恢复，部分实测 | 已恢复并验证 32 个应用的注册；计算器、时钟、图书和音乐可打开。音乐播放尚未验证，提醒事项图标异常暂未复现，见[系统应用恢复](docs/system-apps.md) |

软件刷新通知已尝试提高到 120 Hz，**这不代表面板已在 120 Hz 扫描，也不代表桌面能达到 120 FPS**。当前优化重点是渲染正确性、提交开销和显示输出路径。

## 性能修复与待完成项

当前实现包含 128 段 GPU 批量提交、每次启动自检后启用的缓存一致性优化、缓冲区复用、显示完成唤醒和交互调频。桌面滑动与最近任务曾有实机改善，但**开关应用动画仍卡顿，紫色阴影、偶发文字异常和滑动撕裂尚未解决**。提交、映射和合成线程采样用于定位这些问题；主机检查或单项耗时下降不代表端到端帧率提升。

当前优先工作：

- [ ] 缩短冷启动、连续操作和静置后开关应用的延迟，补齐呈现帧间隔测量。
- [ ] 修复紫色阴影、文字纹理错误、壁纸黑闪与滑动撕裂，验证透明混合和显示同步。
- [ ] 完成 GPU 地址空间、内存压力及 shader 兼容性验证。主客户端已有 2 GiB 窗口，详见 [GPU 地址空间](docs/gpu-address-space.md)。
- [ ] 修复照片图库初始化，完成墙纸、其余系统应用和扩展的功能验证。
- [ ] 降低持续交互发热，补齐电流、电池温度保护及充电控制，详见 [电池接入](docs/battery.md)。
- [ ] 完成亮度范围、熄屏/唤醒、音频与无线网络适配。
- [ ] 完善冷启动恢复和全新环境中的完整构建流程。

实现原理与上游来源见 [性能说明](docs/performance.md)，逐轮设备观察与未通过项见 [设备测试](docs/device-tests.md)。

每项的依赖、下一步和验收条件列在 [待完成项](docs/roadmap.md)，其他开源移植的实际采用范围见 [跨项目对照](docs/upstream-features.md)。小米官方及社区设备树的核对、X4 boost 与 DSI 后续修正见 [设备树对照](docs/device-tree.md)。其他机型的实现用于参考，其实测结果不代表小米 14 已支持对应功能。

## 开发

仓库包含板级源码、固定版本的上游补丁、图形适配和离线检查工具。iOS 镜像、KDK / SDK、厂商固件及分区备份需要自行准备，均不随仓库分发。

```bash
git clone https://github.com/lonzzi/UniXNU-houji.git
cd UniXNU-houji
python3 tools/check-source-tree.py
python3 tools/run-host-checks.py
```

离线检查验证频点策略、HFI、实际提交/页表代码、显示队列、预热调度及缓冲区回收的已知回归，不模拟手机或 GPU。源码组装、构建依赖及实机验证流程见 [开发指南](docs/development.md)。

## 代码导航

| 目录 | 内容 |
| --- | --- |
| [`kernel/`](kernel) | XNU 补丁、八核与调频、UFS、触控、显示和 GPU 内核实现 |
| [`uefi/`](uefi) | houji 板级覆盖层、BootShim 与 Mu_Basecore 补丁 |
| [`bootloader/`](bootloader) | DarwinBoot 的设备树、内存与 GPU 状态交接 |
| [`graphics/`](graphics) | AdrenoMetal 前端、metal2vulkan 适配、Turnip Darwin 后端 |
| [`userland/`](userland) | 键盘几何、导航与墙纸等用户空间适配 |
| [`tools/`](tools) / [`checks/`](checks) | 源码准备、构建入口与离线回归检查 |
| [`docs/`](docs) | 开发、架构、已知问题与恢复说明 |
| [`archive/`](archive) | 历史实验源码与调查资料，供追溯参考 |

当前依赖版本由 [`sources.lock.json`](sources.lock.json) 固定。修改应以各组件的有效覆盖层和补丁为准，历史实验目录不作为默认构建入口。

## 参与贡献

欢迎提交可复现的问题和小范围补丁。反馈请附上源码版本、测试镜像标识、预期行为与实际表现；日志中请移除设备标识及个人信息。提交规范和验证要求见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 致谢与许可

感谢 [UniXNU](https://github.com/UniXNU)、[Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)、[Apple Open Source](https://github.com/apple-oss-distributions/xnu)、[Mesa](https://mesa3d.org/)、[AdrenoMetal](https://github.com/UniXNU/AdrenoMetal)、[metal2vulkan](https://github.com/steelbrain/metal2vulkan)，以及 [HackiOS](https://github.com/oosexgang/hakios-xnu-kernel)、[ios-sweet](https://github.com/dipa-studio/ios-sweet)、[sheng](https://github.com/UniXNU/UniXNU-sheng)、[一加 Ace 5](https://github.com/tycykp/UniXNU-OnePlus-Ace5)、[小米 17 Pro](https://github.com/yechenan/data-a-live)、[Nothing Phone (3a)](https://github.com/rootstockos/UniXNU-asteroids)、[小米 13 Ultra](https://github.com/markakash/UniXNU-ishtar-old) 等移植项目提供的实现和调查资料。

本仓库包含多种许可证的代码，保留各文件及上游原有条款。详见 [COPYING.md](COPYING.md)。
