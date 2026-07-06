# 跳过测试
.\scripts\package-release.ps1 -SkipTests

# 不生成 zip，只生成目录
.\scripts\package-release.ps1 -NoZip

# 指定 Qt/CMake 前缀
.\scripts\package-release.ps1 -CMakePrefixPath "D:\Application\ProgramingIDE\Qt6\6.11.1\mingw_64"

# 发布包内容
发布包只包含 `MicArray.exe`、Qt 运行库/插件和 `RELEASE.txt`，不再打包 `refer/` 与 `doc/` 目录。

# 模拟服务端音频节奏
`tools/mock_device_server.py` 默认使用 `--audio-fps 240`，在每帧 200 个采样点时对应约 `48 kHz` 的模拟采样节奏；联调压力较大时可降低该参数。
