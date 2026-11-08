# EDK II for UMS9620 / W202DS

W202DS 的 UEFI 固件平台。`Ums9620Pkg/` 保存板级适配，`edk2/` 是固定版本的 Tianocore EDK II 子模块；上游代码与本板改动分别纳入版本管理。来源版本见 [sources.lock.json](sources.lock.json)。

## 提供的功能

- 接收 LK 的内存、显示、设备树和返回上下文，建立 SEC/DXE 与 UEFI Boot Services。
- 显示 GOP、eMMC/GPT/FAT、触摸及触摸固件重载、按键、电量/充电、关机/复位和看门狗。
- 编译 Android LK 返回加载器、Debian Server/KDE Linux 加载器及触摸诊断应用。

本仓库不包含完整 LK，也不链接 LK `.a`。启动前的硬件初始化由已有 LK 完成，运行中的板级 UEFI 驱动在这里编译。PocketJS 接口头文件是显式输入；引擎和页面在 `pj4edk2`，最终链接与 LK 镜像封装在 `w202ds-boot`。

## 构建

主机需要 Bash、Python 3.11+、GNU make、GCC/G++、UUID 开发头文件、AArch64 GNU GCC/binutils。推荐使用 GCC 13；本次验证为 13.3.0。

```sh
git submodule update --init edk2
git -C edk2 submodule update --init \
  BaseTools/Source/C/BrotliCompress/brotli \
  MdeModulePkg/Library/BrotliCustomDecompressLib/brotli

./build.sh --output /path/to/build/platform \
  --abi-include ../pj4edk2/pocketjsforedk2/include \
  --touch-firmware ../w202ds-linux/6.18.35/Devices/touchscreen/firmware/Himax_firmware_guoxian.bin
```

`--output` 必须位于源码树外。未设置 `EDK_TOOLS_PATH` 时，构建入口会在输出目录编译一份独立的 BaseTools；也可以先运行 `./build-tools.sh /path/to/build/basetools` 并设置该环境变量。工具链不会由脚本自动安装。

主要输出在 `workspace/Build/Ums9620/RELEASE_GCC5/`：

| 输出 | 用途 |
| --- | --- |
| `FV/UMS9620.fd`、FFS 和 FV 模板 | 平台组件，交给 `w202ds-boot` 加入 PocketJS 后重新生成最终 FD |
| `AARCH64/AndroidLk.efi` | 返回已有 LK 的 Android 启动路径 |
| `AARCH64/DebianServer.efi`、`DebianKde.efi` | 从 ESP 加载对应 Linux 镜像与设备树 |
| `AARCH64/TouchScope.efi` | 触摸诊断应用；默认也在 FV 中 |

平台 FD 还没有 PocketJS 菜单，不能当作完整 `uboot_b` 镜像。芯片入口和内存约定见 [Ums9620Pkg/README.md](Ums9620Pkg/README.md)。

## 测试

```sh
export PJB_BOOT_ABI_INCLUDE="$(realpath ../pj4edk2/pocketjsforedk2/include)"
PYTHONDONTWRITEBYTECODE=1 TMPDIR=/path/to/build/tmp \
  python3 -m unittest discover -s tests -p test_hardware.py -v
```

该测试编译并运行实际驱动 C 代码，检查触摸包、外设状态和生命周期。`test_android_boot.py`、`test_linux_boot.py` 还需要它们声明的真实 OS 镜像输入；它们不属于上述无镜像测试命令。

本次拆分以已部署的 `uefi-controls-test-v1` 对应源码为基线。新仓库的离线验证范围见 [VALIDATION.md](VALIDATION.md)。
