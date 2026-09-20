# 统一发布入口

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

默认 `-Variant Auto` 跟随当前源码的根 CMake 默认配置，不依赖 Git 分支名称：本 `SpiralArray` 分支打包 `MicArray128.exe`；旧源码默认打包 `MicArray.exe`。可显式选择：

```powershell
.\scripts\package-release.ps1 -Variant Array128
.\scripts\package-release.ps1 -Variant Legacy48
.\scripts\package-release.ps1 -PlanOnly                 # 只输出方案 JSON，不构建、不创建目录
.\scripts\package-release.ps1 -SkipTests                # 已有测试结果时才使用
.\scripts\package-release.ps1 -NoZip                    # 只生成发布目录
.\scripts\package-release.ps1 -CMakePrefixPath "D:\Application\ProgramingIDE\Qt6\6.11.1\mingw_64"
```

128 版默认使用独立 `build-array128/`；48 版默认使用 `build/Desktop_Qt_6_11_1_MinGW_64_bit_Release/` 并显式启用 `MICARRAY_BUILD_LEGACY_48=ON`。`-BuildDir` 可更改路径，已有根目录方式配置的 128 构建也受支持；源码/版本不匹配的缓存会报错，不删除、不强行切换。相对路径均相对于仓库根目录，而非当前终端目录。

Qt、CMake 和编译器优先从已有 CMake 缓存恢复；编译器与 Qt 的 bin 目录临时置于 PATH 前部，退出时还原，避免默认 PATH 的 MSYS 工具链混用。新机器可指定 `-CMakePrefixPath`、`-CMakePath`、`-CTestPath`、`-CxxCompiler`、`-MakeProgram`；其他选项包括 `-Configuration`、`-Generator`、`-Parallel`（默认 6）、`-DistDir`。多套 MinGW 无法唯一识别时必须指定编译器，不猜测版本。

`array128/package.ps1` 保留为 128 版快捷入口，委托同一个发布脚本，支持原 `-QtKit`、`-QtTools`、`-SkipTests` 及新增的 `-NoZip`、`-PlanOnly`、`-BuildDir`、`-DistDir`。

## 发布包内容和失败保护

- 默认生成 `dist/MicArray128-Release-时间戳-唯一标识/` 和同名 ZIP；48 版使用 `MicArray-...`。每次创建新目录，不删除或覆盖旧包，也不替换旧的 `dist/MicArray128/` 手工部署目录。
- 仅含对应可执行程序、Qt/编译器运行库、插件与 `RELEASE.txt`，不打包 `refer/`、`doc/`、录音、自测报告或另一版程序。
- `RELEASE.txt` 标明版本、测试状态、EXE SHA-256、运行方式和对应协议。128 版说明 UDP 5000、FPGA `192.168.10.2:5000`、本机 USB 摄像头；不混入旧 TCP 地址。
- 128 版构建并运行 `test_array128`；48 版构建全部六个 CTest 目标再运行，避免只构建三个却调用全部测试。
- 配置、构建、测试、部署任何一步失败立即报错；只有部署成功且主要 DLL/Windows 插件齐全才写清单和生成 ZIP。失败后的独立目录保留供诊断，不自动清理用户文件。
- 发布路径不同于开发路径，Windows 防火墙可能需要针对该 EXE 再授权。脚本不会修改防火墙、网卡或启动硬件接收。

## 脚本回归测试

```powershell
python -m unittest discover -s tests/tools -p "test_package_release.py" -v
```

覆盖默认/显式版本选择、旧源码中残留新目录、根目录 128 构建、缓存保护、快捷入口和三个外部命令失败关口；Windows PowerShell 5.1 下执行。

## 模拟服务端音频节奏（仅旧 48 通道版）

`tools/mock_device_server.py` 默认使用 `--audio-fps 240`，在每帧 200 个采样点时对应约 `48 kHz` 的模拟采样节奏；联调压力较大时可降低该参数。
