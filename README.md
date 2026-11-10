# EDK II for UMS9620 / W202DS

W202DS 的 UEFI 固件平台。`Ums9620Pkg/` 保存板级适配；构建脚本独立获取 Tianocore EDK II 和 Brotli，
默认使用 [sources.lock.json](sources.lock.json) 中的固定提交，并在外部输出目录构建。

## 提供的功能

- 接收 LK 的内存、显示、设备树和返回上下文，建立 SEC/DXE 与 UEFI Boot Services。
- 显示 GOP、eMMC/GPT/FAT、触摸及触摸固件重载、按键、电量/充电、关机/复位和看门狗。
- 编译 Android LK 返回加载器、Debian Server/KDE Linux 加载器及触摸诊断应用。

本仓库不包含完整 LK，也不链接 LK `.a`。启动前的硬件初始化由已有 LK 完成，运行中的板级 UEFI 驱动在这里编译。PocketJS 接口头文件是显式输入；引擎和页面在 `pj4edk2`，最终链接与 LK 镜像封装在 `w202ds-boot`。

## 构建

主机需要 Bash、Python 3.11+、GNU make、GCC/G++、UUID 开发头文件、AArch64 GNU GCC/binutils。推荐使用 GCC 13；本次验证为 13.3.0。

```sh
./build.sh --output /path/to/build/platform \
  --source-cache /path/to/cache/upstream \
  --abi-include ../pj4edk2/pocketjsforedk2/include \
  --touch-firmware ../w202ds-linux/6.18.35/Devices/touchscreen/firmware/Himax_firmware_guoxian.bin
```

`--output` 必须是源码树外的新目录。首次构建自动获取依赖，无需先初始化仓库内的 `edk2/` 子模块。
未设置 `EDK_TOOLS_PATH` 时，构建入口会在输出目录编译一份独立的 BaseTools；也可以先运行
`./build-tools.sh --output /path/to/build/basetools --source-cache /path/to/cache/upstream`
并设置该环境变量。工具链不会由脚本自动安装。

## 选择上游源码

| 名称     | 默认来源                                | 版本规则                                                                                   |
| -------- | --------------------------------------- | ------------------------------------------------------------------------------------------ |
| `edk2`   | `https://github.com/tianocore/edk2.git` | 默认固定为 `e7aac7fc137e247edad22f7ee53b9a1fba227397`                                      |
| `brotli` | `https://github.com/google/brotli`      | 默认固定为 `f4153a09f87cbb9c826d8fc12c74642bb2d879ea`，用于 BaseTools 和 MdeModulePkg 两处 |

可以指定仓库、完整提交、分支、标签，或用 `latest` 选择所选仓库默认分支的 HEAD：

```sh
./build.sh --output /path/to/build/platform-custom \
  --source-cache /path/to/cache/upstream \
  --source-repo edk2=https://github.com/tianocore/edk2.git \
  --source-ref edk2=latest \
  --abi-include /path/to/pocketjsforedk2/include
```

选择其他 EDK II 提交时，Brotli 默认跟随该提交记录的 gitlink。
如果同时重放包含 Brotli 提交的源码清单，则保留清单中的选择。
`--source-repo brotli=URL`、`--source-ref brotli=REV` 可以同时覆盖两份 Brotli；
也可用 `edk2/BaseTools/Source/C/BrotliCompress/brotli` 或
`edk2/MdeModulePkg/Library/BrotliCustomDecompressLib/brotli` 分别选择。
这些选项也适用于 `build-tools.sh`，旧的 `build-tools.sh OUTPUT_DIRECTORY` 位置参数仍可用。

实际提交写入输出的 `upstream.lock.json`，下次通过 `--source-lock FILE` 重放。
固定提交在缓存完整时可离线复用；分支、标签及 `latest` 每次重新联网解析，失败时不会换回其他版本。
缓存可用 `--source-cache DIR` 或 `PJB_SOURCE_CACHE` 指定。Git 使用调用者已有的代理和认证配置，脚本不改全局网络。

每次构建在输出的 `edk2-source/` 准备独立 Git checkout，编译实际选择的 EDK II 和 Brotli，
不修改仓库内子模块或缓存源码。也可通过 `--edk2 DIR` 使用外部 EDK II，此时两份 Brotli 必须已初始化，
且不能同时传入源码清单、仓库或引用覆盖选项。所选源代码应保持干净。

也可只解析或准备依赖：

```sh
python3 tools/upstream.py resolve --output /path/to/build/upstream.lock.json \
  --source-cache /path/to/cache/upstream
python3 tools/upstream.py prepare --output /path/to/build/platform-sources \
  --source-cache /path/to/cache/upstream --source-lock /path/to/build/upstream.lock.json
```

版本可选不代表接口兼容；当前完整构建验证默认固定版本和 AArch64。

主要输出在 `workspace/Build/Ums9620/RELEASE_GCC5/`：

| 输出                            | 用途                                                         |
| ------------------------------- | ------------------------------------------------------------ |
| `FV/UMS9620.fd`、FFS 和 FV 模板 | 平台组件，交给 `w202ds-boot` 加入 PocketJS 后重新生成最终 FD |

ESP 启动器和诊断应用（`AndroidBoot`、`AndroidLk`、`DebianServer`、
`DebianKde`、`HandoffProbe`、`TouchScope`）已移至组合仓库
[`pocketjs4edk2_uefi/efi`](https://github.com/Yeamika/w202ds-boot/tree/main/pocketjs4edk2_uefi/efi)，
由该仓库的独立 DSC 构建，不再进入平台 FV。

输出根目录的 `upstream.lock.json` 保存 EDK II 和两个 Brotli 的实际来源，
`source-paths.json` 保存本次源码位置，`platform-build.json` 校验这些来源、板级代码、配置、ABI 和产物哈希。

平台 FD 还没有 PocketJS 菜单，不能当作完整 `uboot_b` 镜像。芯片入口和内存约定见 [Ums9620Pkg/README.md](Ums9620Pkg/README.md)。

## 测试

```sh
export PJB_BOOT_ABI_INCLUDE="$(realpath ../pj4edk2/pocketjsforedk2/include)"
export PJB_EDK2_ROOT=/path/to/build/platform/edk2-source
PYTHONDONTWRITEBYTECODE=1 TMPDIR=/path/to/build/tmp \
  python3 -m unittest discover -s tests -p test_hardware.py -v
```

该测试编译并运行实际驱动 C 代码，检查触摸包、外设状态和生命周期。原先位于本仓库的
`test_android_boot.py`、`test_linux_boot.py` 已随 ESP 应用移至
[`pocketjs4edk2_uefi/tests`](https://github.com/Yeamika/w202ds-boot/tree/main/pocketjs4edk2_uefi/tests)，
它们仍需要各自声明的真实 OS 镜像输入。

依赖获取与选择测试使用本地 Git 仓库，可单独运行：

```sh
python3 -B -m unittest discover -s tests -p test_upstream.py -v
```

本次拆分以已部署的 `uefi-controls-test-v1` 对应源码为基线。新仓库的离线验证范围见 [VALIDATION.md](VALIDATION.md)。
