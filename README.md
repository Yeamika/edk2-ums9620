# EDK II for UMS9620 / W202DS

W202DS 的 UEFI 固件平台，基于 Tianocore EDK II。当前只支持 W202DS 板型。
板级源码位于 `Ums9620Pkg/W202ds/`，上游版本由 [sources.lock.json](sources.lock.json) 锁定。

## 项目范围

- 接收前级 LK 的内存、显示、设备树和返回上下文，建立 SEC/DXE 与 UEFI Boot Services。
- 提供 GOP 显示、eMMC/GPT/FAT、触摸及触摸固件重载、按键、电池监测、充电控制、关机/复位和看门狗实现。
- 提供可选的 LK Fastboot DXE 适配；需要匹配的前级加载器协议。

各功能的实现范围与验证限制见 [平台说明](Ums9620Pkg/README.md) 和 [验证记录](VALIDATION.md)。

本仓库维护平台组件。PocketJS 引擎、接口与页面由 [pj4edk2](https://github.com/Yeamika/pj4edk2) 维护；
独立 EFI 应用、最终链接及 LK 镜像封装由 [w202ds-boot](https://github.com/Yeamika/w202ds-boot) 维护。
平台 FD 需要经过组合构建，才能生成适用于设备的完整镜像。

## 目录

```text
edk2-ums9620/
├── Ums9620Pkg/
│   ├── W202ds/
│   │   ├── drivers/
│   │   ├── firmware/touch/
│   │   ├── Include/
│   │   ├── Library/
│   │   └── W202ds.fdf
│   ├── Ums9620.dsc
│   └── README.md
├── edk2/                   # 上游子模块；自动构建入口无需预先初始化
├── tests/
├── tools/
├── build.sh
├── build-platform.sh
├── build-tools.sh
├── sources.lock.json
├── VALIDATION.md
└── README.md
```

## 构建

需要 Bash、Python 3.11+、GNU make、GCC/G++、UUID 开发头文件，以及 AArch64 GNU GCC/binutils。
历史构建使用 GCC 13.3.0，目标为 AARCH64 / RELEASE / GCC5。

运行以下命令前，设置三个环境变量：

| 变量 | 内容 |
| --- | --- |
| `BUILD_DIR` | 源码树外的构建根目录；每次平台构建使用一个尚不存在的输出子目录 |
| `SOURCE_CACHE` | 上游源码缓存目录 |
| `ABI_INCLUDE` | `pj4edk2` 提供的公共 ABI 头文件目录，包含 `pocketjs_boot_context.h` |

```sh
./build.sh --target w202ds   --output "$BUILD_DIR/platform"   --source-cache "$SOURCE_CACHE"   --abi-include "$ABI_INCLUDE"
```

入口自动获取 EDK II 和所需的两份 Brotli。未设置 `EDK_TOOLS_PATH` 时，在构建输出中编译独立 BaseTools。
也可单独准备 BaseTools：

```sh
./build-tools.sh --output "$BUILD_DIR/basetools" --source-cache "$SOURCE_CACHE"
export EDK_TOOLS_PATH="$BUILD_DIR/basetools"
```

`--board w202ds` 是 `--target w202ds` 的兼容别名；其他板型会在构建前被拒绝。
触屏固件默认从本仓库获取并校验，详见 [固件说明](Ums9620Pkg/W202ds/firmware/touch/README.md)。

## 上游版本

| 依赖 | 默认仓库 | 默认提交 |
| --- | --- | --- |
| EDK II | [tianocore/edk2](https://github.com/tianocore/edk2) | `e7aac7fc137e247edad22f7ee53b9a1fba227397` |
| Brotli（两处） | [google/brotli](https://github.com/google/brotli) | `f4153a09f87cbb9c826d8fc12c74642bb2d879ea` |

通过 `--source-repo NAME=URL` 和 `--source-ref NAME=REV` 覆盖依赖。
`REV` 支持完整提交、分支、标签及 `latest`；`latest` 解析所选仓库默认分支的 HEAD。
例如在构建命令中追加 `--source-ref edk2=latest`。

选择其他 EDK II 提交时，未显式覆盖的 Brotli 跟随该提交的 gitlink。
`brotli` 名称同时覆盖两处，也可分别指定
`edk2/BaseTools/Source/C/BrotliCompress/brotli` 和
`edk2/MdeModulePkg/Library/BrotliCustomDecompressLib/brotli`。

实际来源写入 `upstream.lock.json`，可用 `--source-lock FILE` 重放。
缓存完整时可离线重放固定提交；分支、标签和 `latest` 每次联网解析，失败时不会切换版本。
以上选项也适用于 `build-tools.sh`；缓存还可通过 `PJB_SOURCE_CACHE` 指定。
脚本使用调用者的 Git 网络和认证配置。

`--edk2 DIR` 可使用调用者提供的干净 EDK II checkout，要求两处 Brotli 已初始化；
此模式不能同时使用源码清单、仓库或引用覆盖选项。版本可选不保证接口兼容。

## 输出

| 输出 | 用途 |
| --- | --- |
| `workspace/Build/Ums9620/RELEASE_GCC5/FV/UMS9620.fd` | 平台 FD，供组合仓库完成最终链接与封装 |
| `upstream.lock.json` | 实际使用的上游仓库与提交 |
| `source-paths.json` | 构建过程中使用的源码位置 |
| `platform-build.json` | 来源、板级源码、ABI、构建选项、固件及产物哈希 |

Android、Debian Server/KDE 加载器及诊断应用在
[w202ds-boot 的 EFI 应用目录](https://github.com/Yeamika/w202ds-boot/tree/main/pocketjs4edk2_uefi/efi)
独立构建，不进入平台 FV。

## 测试

硬件模型测试编译并运行实际驱动 C 代码，需要公共 ABI 与完整的 EDK II 头文件：

```sh
export PJB_BOOT_ABI_INCLUDE="$ABI_INCLUDE"
export PJB_EDK2_ROOT="$BUILD_DIR/platform/edk2-source"
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p test_hardware.py -v
```

上游获取与版本选择测试使用隔离的 Git 测试仓库：

```sh
python3 -B -m unittest discover -s tests -p test_upstream.py -v
```

历史构建、模型测试与设备验证的区别见 [VALIDATION.md](VALIDATION.md)。
