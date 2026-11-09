# 跨项目实现与 houji 适配边界

2026-10-31 核对其他移植项目，2026-11-01 继续核对 UniXNU 图形更新。包括先前可行性调查中的 HackiOS、ios-sweet、ishtar、Ace 5、data-a-live，
以及 UniXNU 当前驱动。下面区分对方已经记录的能力、本次实际采用的代码与本机仍缺的硬件通路。

小米官方 DTS/驱动、houji Android 配置与本机 Recovery 的逐项对照见 [设备树核对](device-tree.md)。
这次补充核对还找到了 X4 厂商 boost 被遗漏，以及 DSI 回包和中断状态保护不足的问题，均已修正候选源码。

## 已用于本次候选

| 来源与版本 | 采用内容 | houji 的差异与验证边界 |
| --- | --- | --- |
| [UniXNU-sheng `08cde20e`](https://github.com/UniXNU/UniXNU-sheng/tree/08cde20edc3254ed6f7df8d0f5384fb887786e8f) | 触控短时顶档、唤醒提升、CPU 交互策略；对照 GPU 提交和锁外等待；CoreBrightness 接口行为 | 使用本机 12 档 GPU 表、4 个 CPU 域；r242 发热后改为有负载时提供交互下限，取消触控必顶档。缓存维护仍需屏障，未复制 SM8550 地址、平板背光或电池参数 |
| [data-a-live `6547194c`](https://github.com/yechenan/data-a-live/tree/6547194c4883efad2f8a42ad46e95874dd5e7576) | TSENS 温度解码/逐级限制思路；0137、0139–0141、0263 的 DSI FIFO、回读和渐变接口 | 本机 TM 窗口、传感器分组、DSI0 和 n3 面板参数来自 houji；命令改为独立定时状态机，有界等待并保存/恢复控制状态；不复制 SM8850 的 NTC 校准 |
| [metal2vulkan `219f0136`](https://github.com/steelbrain/metal2vulkan/tree/219f01365dc4c8eb346816c405b5b17403793764) | 更新翻译器和运行时：设备原生 XR 混合、pipeline 变体复用、host-copy 与 inline render pass 同步修复 | 移除已被上游取代的本地 XR fallback；保留 iOS 退出适配、ASTC/IOSurface 导入、常量折叠与资源依赖补丁。更新缓存版本和运行时 ID，不沿用旧二进制 |
| [Adreno740 `5c8c1453`](https://github.com/UniXNU/Adreno740/commit/5c8c1453c67d6ef6b10ec43ab1f8fc570a2f33e1) | 提交阶段减少重复 TLB 失效和诊断映射 | 保留本机每次缓存发布、页属性检查、unmap 同步及周期性 IB 审计 |
| [AdrenoMetal `f0c31774`](https://github.com/UniXNU/AdrenoMetal/commit/f0c317742b08379bab3239b5558dcc16f8c22f66) | shader 清单、运行时/Turnip 缓存身份、零值常量与关联函数预热 | 本机内嵌 iOS 16 的 138 条路径，优先界面库；另检查未完成命令，连续繁忙不强行恢复，OOM 停止当轮。r227 前未安装清单，预热没有启动 |

r227 后另对照上游的 1 GiB 渲染地址窗口和 metal2vulkan 的 64 MiB 池：新增同堆空闲块回收、按实际大小分配回退和 Turnip 碎片统计。上游并未解决本机共享 GPU 页表和客户端数量限制；8 GiB RAM 不会自动解除它们。详见[内存与预热](performance.md#8-gib-ram-与-gpu-分配不足)。

DSI 适配保留 APSL-2.0 标识，文本见 `kernel/APPLE_LICENSE`；其他文件保留原有许可。
这些修改已进入有效覆盖层。编译、策略测试和主机 shader 测试不等于 A750 渲染或面板实测。

## 2026-11-01 图形更新

本次实际拉取 AdrenoMetal `831a9dcb`、Adreno740 `a9a2cbb4` 和 ArmSmmu4K `83969cd8`。
metal2vulkan 主库仍为已经接入的 `219f0136`；新增修复主要在 AdrenoMetal 的桥接代码和运行时补丁中。
以下内容已适配到有效源码并完成 iOS 16 Metal bundle 构建，设备验收另记。

| 上游改动 | 本机采用方式 | 尚不能据此声称的结果 |
| --- | --- | --- |
| [旋转、resolve、间接计算 `22f4dbdb`](https://github.com/UniXNU/AdrenoMetal/commit/22f4dbdb964edde4aba9d202aceae60bc43fb1a0) | 透传纹理 rotation/framebufferOnly/isDrawable；保留本机 swizzle；颜色、深度、模板 resolve 目标及索引/过滤器完整传入，目标保留到命令完成；间接计算参数交给 GPU 读取 | 确认修正了漏传参数，但尚未确认紫色或乱码由此产生，也不是 A750 像素回读结果 |
| [内存预分配与图像复用 `e0997b08`](https://github.com/UniXNU/AdrenoMetal/commit/e0997b081db96863ba7fe8ea9d9527e7b1607161) 的 0013/0021 补丁 | 池从 4 MiB 逐步增长，单块上限仍 64 MiB；后台线程合并预分配请求；已退休的专用图像块复用 10 秒。本机另限每设备 4 块 / 128 MiB，OOM 按同堆回收并保留实际大小回退，系统内存压力也可清空 | 不能用主机分配测试证明每帧耗时；这是用户空间池，区别于 r271 的 512 MiB 内核 BO 池 |
| [按函数续编与 RGhA 预热 `1d19b759`](https://github.com/UniXNU/AdrenoMetal/commit/1d19b7596d443431df423c08ae88f31f47f260b4) | 新增 `prime-fn`，以库路径/大小/纳秒 mtime、运行时和 Turnip 身份绑定已成功函数；补 RGBA16Float 变体；每个函数结束清理中断标记；保留操作繁忙时让路及 OOM 暂停 | 冷缓存仍需逐步建立，不表示所有 Core Image 私有库均支持，也不能保证首轮动画消除卡顿 |

主机检查直接执行实际池的分配/退休代码、后台工作线程和桥接转换，覆盖跨语言布局、
导入内存排除、缓存上限/过期/压力回收、合并请求和最后一个引用在工作线程释放。
完整 iOS 16 构建与这些检查仍不能替代显示验收。

[最新 GPU 指南](https://unixnu.com/zh/porting/gpu)仍记录 BGRA8 的 GMEM 颜色问题，
本轮候选恢复现有的 `sysmem,nocb` 默认值，使用与 r271 相同的 Turnip 二进制；不把上游称为已经完全修好。
上游的 iOS QoS 修正已经存在于本机，r271 日志也观察到实际线程 QoS，因此没有重复接入。

Adreno740 的多 ring 抢占不能直接覆盖：上游新增优先级调用占用 method 17，
本机该编号用于 128 段批量提交，且 A750 还需独立验证寄存器、上下文保存与故障回退。
其新的映射索引/页表遍历优化也依赖不同的内存对象结构；本轮保留已实测的内核映射和退休规则。
物理 TE/vblank、持续 120 FPS、紫色和文字正确性仍是独立验收项。

r272 已进入桌面，但用户确认动画仍卡；完整日志已回收，见[设备测试](device-tests.md#r272--接入上游内存和桥接更新动画仍卡)。
后续再采用 `45995840` 中的 `0011` 双平面影子图像优化：其首次复制不裁剪区域，
完整写入两平面的 texel，或由整面 clear 覆盖，故省去创建时不会被读取的初始化。
普通应用纹理、资源依赖和 copy-in/copy-back 保持原契约，尚需实机确认颜色和动画。

同时适配 [命令阶段追踪 `69712c0a`](https://github.com/UniXNU/AdrenoMetal/commit/69712c0ab2f8eb75451a29a1a76d22858b71dd4f)
及当前 `0024` 的提交分段；本机增加不等待的槽位互斥，避免环形覆盖时混读记录，
明确记录丢失和地址复用。`tools/build-metal.py --gpu-stage-trace` 仅在 backboardd 打开，
后台输出至缓存内 `stages.bin`，每次进程启动最多 16 MiB / 360 秒，默认构建关闭。
`tools/analyze-gpu-stages.py` 分析的是命令缓冲阶段，不把它换算成显示帧率。

当前 AdrenoMetal 的旧 `0012` sRGB 补丁仍针对更早的翻译器；本机 `219f0136` 已采用
新的 XR/native blend 路径，原 `SrgbForm` 拒绝分支已不存在，因此没有强行回退再套补丁。
Core Image 私有库加载/拼接仍需完整的前端和系统编译器链，预热拒绝日志本身不能定位紫色。

另外补齐 [`4d789e9c` 的 IOSurface 使用计数](https://github.com/UniXNU/AdrenoMetal/commit/4d789e9c369c8daff06854432f5328ad515a1058)：
只在 backboardd 为导入纹理及视图登记使用，释放纹理时配对撤销；现有命令所有权保留到完成。
仅持有 Core Foundation 对象与向 IOSurface 缓冲池登记使用是不同的操作。本机内核已有计数 trap，
这次补齐前端调用，保留现有 runtime/cache 身份；主机的真实 IOSurface 生命周期检查通过，
尚不能称为实机紫色或乱码修复。上游强引用导入缓存仍单独评估，未照搬其无限容量设计。

r273 阶段与内核分配日志使用同一 uptime 时钟：两次最长回调等待与 64 MiB 分配失败重合，
失败的 backing 阶段分别约 36.130 / 46.038 ms。本机在池满后仍对大对象先试连续物理内存，
因此另适配 [Adreno740 `9e21f6a1`](https://github.com/UniXNU/Adreno740/commit/9e21f6a1ecfcd3ae5fb206bd0b595fadbf9ed8b6)：
池存在时跳过可选的 XNU 连续内存搜索，直接采用已有的分散物理页回退；明确要求连续内存的请求、
CPU 4K/16K 所有权、清零、缓存属性与 SMMU 校验不变。真实分配/客户端代码的主机测试在耗尽池后
确认不发起该搜索，并验证显式连续请求仍失败，不伪装成功。此项不能保证其他分配、映射不再阻塞。

同次核对的 `4c75ccbf` 将系统内存申请移出锁，但依赖上游新的 pageable BO 描述符路径；
本机仍使用已验证的 WC 池和完整 CPU 页所有权，故没有把这套内存模型一并替换。

## 其余项目实际能提供什么

| 项目 | 核对结果 | 对本机的用途 |
| --- | --- | --- |
| [HakiOS（原 HackiOS）`a3384e03`](https://github.com/oosexgang/hakios-xnu-kernel/tree/a3384e031eb1e605cf37adaefc19991f04f6274b) | redwood 记录了 GLES 图形、声音、电量、Wi-Fi 和相机；Metal 还未绘制，充电/蓝牙未完成 | 参考 ADSP 启动、音频和电量服务的组织方式。SC7280/SM7325 的功耗域、共享内存、固件及音频拓扑不能用于 SM8650 |
| [ios-sweet](https://github.com/dipa-studio/ios-sweet) | 同代 iOS 16 / XNU 8796 的用户空间与 framebuffer 基础；README 明确无 GPU，电量服务为静态值 | 有助于 Darwin 22 ABI 与系统服务；不能用静态 100% 或充电图标充当真实电池支持 |
| [UniXNU-ishtar-old](https://github.com/markakash/UniXNU-ishtar-old) | 目标 SM8550、iOS 18 / XNU 11215；公开记录仍在 UEFI 和内核构建阶段 | 参考设备树、UEFI 和来源管理；没有可直接移植的完整音频/无线/Metal 实现 |
| [Ace 5 `aaa100d1`](https://github.com/tycykp/UniXNU-OnePlus-Ace5/tree/aaa100d19f6fb14e5897a7e496edf7838d4d4e9f) | 同 SM8650，含 PCIe/MHI/QMI 和音频分层；音频平台 lease/PCM provider、无线固件启动仍有未解决项 | 可复用协议边界与同 SoC 调查，不能将接口骨架标成可出声或已联网 |
| [Ath12kWifi `f6bf561f`](https://github.com/UniXNU/Ath12kWifi/tree/f6bf561f92cc42761bdd417fce1d53fe1db9477c) | sheng 已有连接网络记录；驱动需要板级 PCIe、DMA、固件和中断接口 | houji 的 WCN7850 仍需 RC0/PHY/SMMU 与 MHI 启动链，单独复制 ath12k 不会产生网络 |

原 `markakash/UniXNU-ishtar` 路径本次返回 404，以上使用仍可读取的 `-old` 仓库。
原 ShueiTsuki 的 data-a-live 已转向 yechenan 仓库。

## 本机 Recovery 核实的输入

本次连接为 `houji` 的 root OrangeFox；Recovery 可读这些信息，不要求 Android 系统存在。
原始设备树及日志留在私有目录，不提交设备标识或固件。

- GPU 是工厂 12 档 231–903 MHz；903 是 governor 上限，不是固定频率。HFI 初始交接仍使用 500 MHz。
- TSENS v2 的三个 TM 窗口为 `0x0c228000`、`0x0c229000`、`0x0c22a000`；CPU 域和 GPU 分组按本机 FDT 绑定。Recovery 能读取 CPU/GPU 温度，但这不证明 XNU 启动后有效位仍保持。
- 面板为 n3_42_0d_0a，DSI0 位于 `0x0ae94000`；硬件范围 0–4095，本机正常亮度上限 2866、最小非零 25，DCS 亮度高字节先发。
- 电池绑定 `qcom,battery-charger` / `qti_battery_charger`，通过 PMIC GLINK；Recovery 可读取实时容量、电压、电流和温度。
- 扬声器有两颗 CS35L41（I²C 地址 `0x40` / `0x42`），另有 WCD939x；不是 sheng 的音频硬件组合。

## 尚需完成的真实驱动工作

r263 已接通 houji 的 ADSP PAS、SMEM/SMP2P/IPCC/GLINK 和本机电池服务，用户确认
真实电量显示及 USB 插拔状态切换。已从小米对应驱动确认容量 4、电流 9、恒定充电电流 12、温度 13 等编号及单位，
与 sheng 的这些基础编号相符；电流符号仍需结合本机充放电状态验证。
sheng 初始化可能设置 USB 输入限流，不能只凭基础编号相符就复制写请求。

声音在这条 DSP 通路上还需要 GPR / AudioReach、WCD939x 路由、双 CS35L41 的匹配固件及
时钟/功放配置。其他项目仅有 ADSP ready、PCM 数据被接收或音频服务注册都不足以证明实际出声。

Wi-Fi 依赖本机 PCIe RC0/PHY、WCN7850 供电复位、SMMU SID `0x1400/0x1401`、DMA、MHI 和 QMI。
蓝牙另需串口/HCI/固件链；蜂窝另需 modem 启动、协议和 iOS 电话服务。
这些尚未接入有效内核，README 保持未完成，不能用空实现、固定数值或服务注册消除待办项。

墙纸、照片仍需新图形候选的应用级复测。Safari 在 `houji_srgb=1` 下已能绘制本地页面（见 [已知问题](known-issues.md)），联网仍依赖无线。
本次优先交付可构建的 GPU、显示和结温修复，剩余功能的验收动作见 [待完成项](roadmap.md)。


r274 映射长停顿复核发现：本机已有 `NativeAdrenoVmAlias` 与 ArmSmmu4K 的排序检查，
但 arena 的 `alias_scratch` 仍为空，两者均退回两两比较。选择性适配
[Adreno740 `d8347316` 的 VM 工作区所有权](https://github.com/UniXNU/Adreno740/commit/d834731657ec66b6a48944896e7fbcc144c422ac)，
申请与固定 span 预算相同的索引空间，供串行的 preflight/页表事务复用。
保留每项别名检查，不启用 Exclusive 映射或动态页表。
候选 r276/r277 均未进入桌面，r278 关闭早期工作区后恢复桌面。后续改为在联合自检、缓存
一致性和池初始化后、provider 发布和图像回读前启用，并要求没有映射客户端对象、前后完整
审计。r280 在后置启用后通过图像检查，但首次调频因 `FHR_BINDING` 退出；主机重现了
工作区字段改写本机封存 arena 的冲突。当前候选只在调用内的局部 arena 视图挂接工作区，
原始描述保持只读，完整性校验不变；旧实现失败、修正通过的回归覆盖实际封存代码。
r281 已确认恢复桌面、索引检查 2227 次、映射最长 8.220 ms，调频改变 544 次且无失败；
用户仍反馈动画卡顿。下一步按上游的逐段时序方法核对锁竞争与完成路径，不能把局部开销下降当作动画改善。

显示诊断按本机 pineapple DTS 的 MDSS 相对偏移和原厂 `sde_hw_intf.c` 的计数器定义适配，
参考[上游显示指南](https://unixnu.com/zh/porting/display)先读交接状态再改变扫描源的顺序。
默认关闭，仅 `-ht` 在首个 GUI 复制完成后读取五次 DSI0 计数；不复制 sheng 的
刷新请求、寄存器写入或双 DSI 假设，也不以硬件扫描计数代替实际渲染 FPS。
