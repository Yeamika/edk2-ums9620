# UMS9620 / W202DS 平台

构建入口为 [Ums9620.dsc](Ums9620.dsc)，当前只支持 W202DS。
板级驱动、固件、头文件、平台库与 FV 描述集中在 `W202ds/`。

## 组件

| 组件 | 作用 |
| --- | --- |
| `W202ds/drivers/BoardServicesDxe/` | 触摸、按键、电池监测、充电控制及看门狗生命周期 |
| `W202ds/drivers/DiagnosticBds/` | 初始化启动阶段、连接控制器并启动菜单；启动外部应用前发送 EndOfDxe |
| `W202ds/drivers/EmmcDxe/` | 继承 SDHCI 控制器，提供受写入边界限制的 BlockIo |
| `W202ds/drivers/LkGopDxe/` | 继承显示与 GOP 操作 |
| `W202ds/drivers/LkFastbootDxe/` | 可选的、基于版本化 LK 服务描述的 Fastboot 适配 |
| `W202ds/Library/LkInheritedPlatformLib/` | 平台入口、内存资源与继承环境 |
| `W202ds/Library/RamTraceSerialPortLib/` | RAM 跟踪日志 |
| `W202ds/Library/Ump9620ResetSystemLib/` | UMP9620 原生关机及复位接口 |
| `W202ds/firmware/touch/` | 固定版本的 HX83102J 固件与校验清单 |

## 入口与内存约定

前级加载器必须初始化 DDR、时钟、供电、面板及持续扫描输出，并保留有效的 LK handoff 记录。
公开的 SoC 参考源码不包含完整的 W202DS 面板初始化配置。

- 仅主核 MPIDR 0 进入，其他 CPU 保持静止；使用 AArch64，异常级别及 MMU/cache 状态满足 ArmPlatform PrePi 的 SEC 入口要求。
- UEFI 资源共 128 MiB：`0x82800000..0x85ffffff` 为 56 MiB 引导区，`0xc8000000..0xcc7fffff` 为 72 MiB DXE 扩展区，均采用 write-back 映射。
- 引导区包含 2 MiB FD、PrePi 工作区及保留页，可分配空间小于资源总量。不得扩展至 `0x86000000` 的 LK 下载区，或向 UEFI 分配器暴露 `0xd0000000` 的 OS staging 区。
- handoff 记录位于 `0x82ffe000`，trace 位于 `0x82fff000`；加载器分别初始化，PEI 保留这两页。handoff v2 为 48 字节，v1 继续支持 FGU 数据。
- 帧缓冲位于 `0xb0000000`。GOP mode 0 为 1200×1920 RGB565，stride 2400 字节；既有 EFI/OS 交接使用此格式。
- 可选 mode 1 使用相同几何尺寸的 32-bit BGRX，stride 4800 字节。切换保留像素、面板配置及旋转；进入既有 OS 加载器前必须恢复 mode 0。此模式仍需板级验证。

前级加载器负责定位并进入重定位后的 SEC。原始 FD 不能直接作为 uboot 镜像刷写。

## 功能与验证范围

| 功能 | 已有记录 | 尚待验证或限制 |
| --- | --- | --- |
| SEC / DXE / 菜单运行 | 历史 RAMBoot 已进入 SEC、DXE 与独立链接的 PocketJS 菜单 | 当前源码的修改需要对应构建与设备验证 |
| 触摸 | 2026-11-05 固件重载后记录 21 次有效 IRQ 输入、0 空报文；用户确认常驻版强制重启前后正常 | 彻底断电及长期使用未验收 |
| 按键 | 音量键选择与电源键确认已有用户验收 | 依赖继承的 EIC/GPIO 状态，不重新配置 pinmux |
| 电池监测 | 有电压、有符号电流、外部输入、NTC 温度及充电状态实测记录 | SOC 使用继承电量及库仑变化估算，不重建 Android 滤波及离开 UEFI 期间的状态 |
| 充电控制 | 已有双通道停充与主路受限开启实现 | 拔插恢复待验；没有电池旁路或独立于固件运行的充电安全监督器 |
| 原生关机 | 有界 PMIC 关机序列已有实现 | 尚未完成硬件验收；历史黑屏但背光未灭的问题未闭环 |
| eMMC / ESP | 有读写、跨簇写入及复位后回读记录 | 写入仅限经 GPT 校验的唯一 ESP；详见 [eMMC 说明](W202ds/drivers/EmmcDxe/README.md) |
| Fastboot 适配 | 已有编译及模型验证记录 | 需要匹配 LK 协议；尚未硬件验收 |
| UEFI 变量 | 提供易失性变量 | 持久 UEFI 变量仍未完成；应用设置由独立运行时通过 ESP 管理 |

## 触摸与生命周期

平台默认校验并嵌入 [触屏固件](W202ds/firmware/touch/README.md)。
启用输入前，检查固件分区映射和软件 CRC，停止触摸 MCU，以有界 SPI 传输写入 SRAM，
校验硬件 CRC 后启动正常报告。每次 UEFI 启动执行一次，上传失败不安装 Absolute Pointer 协议。

触摸 SPI 读取由 IRQ 事件触发，不在周期输入轮询中读取或重载固件。
启动 EFI 子应用前暂停触摸，子应用返回后恢复，ExitBootServices 时永久停止。
阶段码仅表示上传进度，不能代替有效输入验证。

音量加使用 EIC164 高电平，音量减使用 GPIO7 低电平，电源键使用 EIC161 低电平并映射 Enter。
启动时已按住的键被忽略，30 ms 防抖后每次按下产生一个事件。

## 充电、关机与看门狗

充电控制默认停止两颗充电芯片。显式开启只允许主路，并检查继承配置、电池存在、温度、电压和故障状态。
驱动周期复查条件，故障时锁存停充请求；向子应用交接前必须确认停充。
保留的充电器配置关闭了芯片看门狗，固件活性看门狗不能替代充电安全监督器。
ADC/I2C 读取后恢复原寄存器，恢复失败则停止使用对应外设。

Boot Services 阶段原生关机清理 RTC 中断、解锁 PMIC 电源控制并请求下电，失败时有界返回；
OS runtime 复位保留 PSCI 路径。

匹配的常驻加载器通过 handoff 标志启用 PMIC 看门狗续期；
退出或 quiesce 后停止续期，以保留故障恢复能力。RAMBoot 观察窗口由加载器配置。

## 组合与 OS 启动

公共 ABI 由调用者通过 `--abi-include` 显式提供，来源为 [pj4edk2](https://github.com/Yeamika/pj4edk2)。
引擎和页面不在本平台编译；最终链接、镜像压缩与 LK 容器封装由
[w202ds-boot](https://github.com/Yeamika/w202ds-boot) 完成。

Android LK 返回加载器、直接 Android 加载实验、Debian Server/KDE 加载器及诊断应用
均由组合仓库独立维护。历史 Android LK 返回路径已有 RAM 和常驻启动记录；
这些记录不代表本仓库当前平台构建或所有 OS 加载路径已通过设备验收。

默认 FD 为 2 MiB。原始 FD、Android wrapper 与 PLK1 下载包均不能当作可刷写的 uboot 镜像；
设备安装使用组合仓库生成、与对应前级加载器及 ABI 匹配的完整容器。
