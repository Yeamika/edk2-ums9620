# W202DS 触屏固件

`Himax_firmware_guoxian.bin` 是 W202DS 的 HX83102J 触屏固件。
默认构建使用本目录中的固件，在编译前按 [manifest.json](manifest.json) 校验大小与 SHA-256。

| 项目 | 值 |
| --- | --- |
| 大小 | 261120 字节 |
| SHA-256 | `bdcd327d48628dd703b1d0a8ea778ef7f7aacd7beb227efdb78ea788928f0156` |
| 适用板型 | W202DS；未验证其他 UMS9620 设备 |

固件经 `W202ds.fdf` 嵌入 FV，由 `BoardServicesDxe/Touch.c` 和 `TouchReload.c`
加载到触控芯片。`--touch-firmware FILE` 可覆盖输入，但文件仍须匹配当前支持的大小与哈希。

构建直接使用本仓库中的固件，不依赖其他工程的文件布局。
