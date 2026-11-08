# UMS9620 eMMC BlockIo

继承原厂 LK 已初始化的 SPRD SDHCI-r11 eMMC 控制器，向 UEFI 提供 BlockIo。不切换 eMMC boot/RPMB 区，不重设 HS400、时钟、供电或 DLL；写入限制在验证过 GPT GUID 的 ESP 范围内。

## 读取与写入

读取通过 SDMA：512 字节请求使用 CMD17，连续请求分成最多 32 KiB 的 CMD18，并按原厂 LK 的顺序发送 CMD12 停止。私有 bounce buffer 按 64 KiB 对齐，控制器也配置 64 KiB 边界，单次传输不会触及 DMA 重启边界；BlockIo 调用者无需对齐。写入仍为单扇区 CMD24，启用 eMMC cache 时 Flush 发 CMD6 并检查后续响应。

读出的数据只有在传输和停止命令全部成功后才复制给调用者。控制器出错后关闭本次启动的 BlockIo 服务；恢复命令/数据线，不重新初始化卡。来源为平台所继承原厂 LK 的 SDHCI/MMC 事务，移植代码独立维护在本项目。

## 构建与验证

随 Ums9620Pkg 构建为 `EmmcDxe.efi`，输入是 LK handoff 记录和继承的控制器状态，输出为 UEFI BlockIo 协议。项目 `tests/emmc_read.c` 模拟实际驱动的 SDMA 数据、非对齐调用、跨 chunk 边界、CMD12 失败、超时、ESP 写范围及寄存器恢复；不是物理 eMMC 时序证明。

单扇区路径已有 ESP 读写实机记录。2026-11-05 多块读取通过主机测试及真机对照：约 41 MB 的内核与 initrd 加载流程从 5187.399 ms 降至 493.834 ms；其中内核读取 2036.799 → 122.222 ms，initrd 读取 2901.672 → 175.438 ms。重复启动约 494 ms，载荷长度、CRC 检查通过并进入 Linux。计时包含六个加载阶段，不包含菜单动画或 ExitBootServices 后的内核启动时间。
