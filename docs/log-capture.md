# 日志采集

参考 [UniXNU 的日志抓取方案](https://unixnu.com/zh/porting/log-capture)，本机将启动固件、XNU 检查点和 iOS 应用崩溃报告分别保存，并按测试标识归档。二维码 AdvancedLogger 适合 UEFI 阶段；退出 Boot Services 后的系统运行仍需要另外的持久化通路。不能把其他机型的预留偏移、分区布局或看门狗代码直接用于 houji。

## Recovery 中读取原始日志

先回到本机 OrangeFox，使用已经建立且只绑定目标手机的独立 ADB server。工具要求显式设备和端口，不会主动重启 server、挂载分区、重启手机或写设备。不要为采集日志停止其他会话的 ADB server。

```bash
python3 tools/capture-recovery-logs.py \
  --adb /path/to/adb --adb-port 15057 --serial "$HOUJI_SERIAL" \
  --run-id "$HOUJI_RUN_ID" --output .local/logs/current-recovery
```

`HOUJI_RUN_ID` 是本轮布局中的 32 位小写十六进制 UUID，不是 r237 之类的显示名称。`HOUJI_SERIAL` 只用于选择设备，不写入报告。输出目录必须不存在。

工具检查 root、houji Recovery 标记、logfs 的设备节点与 8 MiB 大小，再读取整个分区，比较手机读取前后及本地副本的 SHA-256。输出包括：

- `logfs.bin`：原始 8 MiB 副本，保留后续重解码能力。
- `UniXNU.log`、`XnuLog.bin`：按已验证 FAT12 布局提取的原始文件。
- `XnuLog.txt`：仅在本轮 UUID、提交状态、头部 CRC、正文 CRC 与边界全部通过后生成。
- `capture.json`：校验结果、文件哈希、簇链、采集时间及失败原因。

旧测试、损坏或未完成的记录会返回失败；已经读出的原始数据仍保留。固件文本中的旧 r 编号是历史段落标签，不能替代 XNU 的 UUID 校验。离线重解码不接触手机：

```bash
python3 tools/capture-recovery-logs.py \
  --input .local/logs/current-recovery/logfs.bin \
  --run-id "$HOUJI_RUN_ID" --output .local/logs/current-redecoded
```

## 提取本轮应用崩溃报告

保存启动前和返回 Recovery 后的系统卷快照，校验后在主机挂载分析副本，优先只读挂载；原始快照保持不变。若需回放 HFS 日志，先克隆镜像，只回放可丢弃的分析副本。以下工具只读取已挂载的目录，不负责备份、挂载或写回系统卷：

```bash
python3 tools/capture-userland-logs.py \
  --root /path/to/post-boot-mount \
  --baseline /path/to/pre-boot-mount \
  --run-id "$HOUJI_RUN_ID" --output .local/logs/current-userland
```

工具只收集 root/mobile 日志目录里的 `.ips`、`.crash`、`.panic` 和 `Houji*.log/txt`。使用内容哈希比较本轮新增或改变的文件，不依赖手机时间；保留原文件，并为可解析的 IPS 提取异常及故障线程前 20 帧。损坏的 IPS 也保留。默认每文件上限 16 MiB，总检查上限 256 MiB；拒绝符号链接和特殊文件，遇到限制会报告 `PARTIAL`。不指定 `--baseline` 时只能得到当前文件集合，不能证明它们来自本轮。

IPS 自身没有测试 UUID，报告中的 UUID 是操作者提供的关联标签；必须同时保留对应的启动前后快照和镜像校验记录。输出目录权限为 0700、文件为 0600。报告可能含应用和个人信息，保留在 `.local/`，不要直接提交或自动上传。

## 当前覆盖与限制

r237 已用上述工具从 Recovery 完整读取 8 MiB logfs，验证读取前后稳定、CRC 和本轮 UUID；系统卷比较检出 7 份新增或改变的诊断文件，其中两份 Preferences 崩溃报告指向通知图标生成时的 Core Image OpenGL ES 路径。

当前持久化 `XnuLog.bin` 总长 64 KiB，其中 4 KiB 头部、最多 60 KiB 正文；正文包含 GPU/显示摘要及内核消息尾部，并非整轮完整控制台。读取整个分区不会补回未写入或被覆盖的历史。应用报告也只覆盖系统成功落盘的崩溃。完整连续控制台、早期锁死前的最后日志及实机 USB 调试通道仍待实现或验证。

相关主机检查验证解析器、内容比较、损坏/陈旧记录拒绝、读取边界和目标选择，不仿真手机日志写入或 GPU 行为。


## 定向栈保留

r240 证明普通消息环形缓冲会在退出前被大批统计覆盖，即使采样已启用，最终日志也可能没有调用栈。当前候选在采样路径中另存最近三份进程快照，每份最多 4 KiB，保留完整行并标出遗漏；锁内仅复制内存，不读用户栈、不分配、不做 IO。保存检查点时先拷贝这些快照和 InterfaceNamer 的注册/打开/拒绝/命名请求计数，再使用剩余空间拷贝普通消息尾部。仍是原先 16 个预留块、64 KiB 文件和相同 CRC/提交协议，不能称为完整系统日志。

r241 已从最终日志恢复两份完整 Preferences 采样，均定位到网络接口枚举等待 IOKit 静止。网络快照同时确认服务注册、打开均成功且根注册表仍忙碌。符号化时按 `SMP CACHE capture=` 分开处理各份采样，不能只保留最后一个同名进程；当轮的系统卷内容比较未检出新增 Preferences 或壁纸进程崩溃报告。

r242 的网络摘要已经显示 `registry-busy=0 kextd-required=0`，用户确认 About 首次进入明显加快。该轮交互热问题也由 TSENS 和 GPU 请求驻留保留下来，但显示耗时的普通日志被覆盖。当前后续候选在原有 4 KiB 显示前缀中保存通知、复制、队列和最近四个时间窗口；另保留最多 1 KiB 的 CPU 最近 5 秒完整窗口。前缀使用完整行，内核消息尾部相应缩短；文件仍为 64 KiB、16 个既有预留块，不扩大分区写入范围。CPU/显示统计不是功率测量或面板 FPS。

## 动画等待链

`tools/build-metal.py --gpu-stage-trace` 的诊断构建仅在 backboardd 记录命令阶段，
后台写入 `m2v/stages.bin`，上限为 16 MiB / 360 秒。它记录主机 uptime，GPU 项只有
时间跨度；未校准的 GPU 起止点不能用于拆分排队和完成唤醒。强制返回 Recovery 前
没有落盘的记录也可能缺失，分析必须检查实际时间覆盖，不能只看配置预算。

```bash
python3 tools/analyze-gpu-stages.py /path/to/stages.bin --output .local/stages.json
```

`single_batch_chain` 只分解同一条成功单批命令的相邻阶段；缺失、重复、分组、分批或
逆序记录会单独列为排除项。`creation_to_commit` 包含调用方持有命令而未提交的时间，
不能全部归为 CPU 渲染。命令缓冲数和命令回调延迟均不能反算面板 FPS。

内核候选另提供默认关闭的 `gtrace=1`，只观测原有 issuer mutex。记录保存请求、
取得和释放时刻、获取锁的 `NativeAdrenoPublish.cpp` 源码行、ABI 方法、客户端槽位、
等待/提交 fence 或 BO 句柄，以及释放锁时已发出/已观察到的 fence。未指定的方法、
客户端用 `4294967295` 表示。记录不改变锁顺序、fence、频点和缓存策略。
同一锁可能经过多个持有者后才让等待者进入，不能把最近一位持有者当成唯一原因。

固定内存保存 64 条最近的长区间及最长持有/等待各 3 条，阈值 1 ms；检查点最多输出
12 条去重记录。GPU 摘要预算从 16 KiB 扩到 20 KiB，内核消息尾部相应缩短，
仍只写既有 64 KiB 文件及相同的 16 个预留块。未保留下来的持锁区间不能当作不存在。

```bash
python3 tools/analyze-gpu-issuer.py /path/to/verified-xnu.txt \
  --source /path/to/trial-source/NativeAdrenoPublish.cpp \
  --stages .local/stages.json --output .local/issuer.json
```

两个输入必须属于同一个已校验测试标识，源码必须是该次编译的固定副本；解析器会保存
源码 SHA-256，但不能自行证明输入的测试身份。时间重叠仅帮助选择需要检查的区间，
不是完整因果证明。`gtrace` 是启动参数，组装时仍需检查 Darwin 22 的命令行总容量，
不能直接追加到已经占满的命令行。此候选已经编译，尚未在设备启用。

## Panic 日志（`houji_panic_log=1`）

USB 控制台随 panic 一起断开，PMIC 只记录 `PS_HOLD` 硬复位，因此 panic 原文原本会丢失。启用 `houji_panic_log=1` 后：

- 启动时（根卷挂接后）读取 logfs 元数据，确认 `XnuLog.bin` 的 FAT 分配不变，再装好钩子；日志显示
  `HOUJI PANIC LOG: armed`。
- SWEET 没有固件 panic 区域（设备树无 `pram`），`SavePanicInfo` 原本直接返回，panic 只打印一行。现在
  `pe_init.c` 改用 64 KiB 内核缓冲区，因此会生成完整报告（CPU 状态、回溯）。该缓冲区不跨复位保留。
- panic 走到 `halt_all_cpus` 时、PSCI 复位之前，UFS 驱动把最多 24 KiB 报告和控制台环形缓冲区的尾部写入
  `XnuLog.bin` 的 16 个保留簇：先写正文，最后写头部，每段后执行 SYNCHRONIZE CACHE，不回读。其他 CPU 已停止，
  可能停在 UFS 命令中途或持有 `gHost.lock`，所以不加锁；进行中的命令最多等 2 s，仍未完成则不保存。
- 下一次 UEFI 启动会把 `XnuLog.bin` 重置为初始镜像，因此必须在 panic 后、再次启动 iOS 前，在 Recovery 中读取。
  当前 OrangeFox 没有 `ro.houji.native_rescue`，先用 `dd` 复制 `/dev/block/by-name/logfs`，再用
  `capture-recovery-logs.py --input` 离线解码（`perf-test/panic-test.sh` 为完整流程）。
- 已验证（2026-11-09）：USB 控制台命令 `panic` 触发后，从 Recovery 读出完整报告和控制台尾部，根卷头部保持有效。
  报告打印回溯之后会出现嵌套 panic（`sleh_synchronous_sp1`，推测为 panic stackshot），此时报告和保存都已完成。
