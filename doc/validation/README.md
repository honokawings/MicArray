# 发布验证摘要 / 2026-10-21

## 本次公开源码重新验证

| 项目 | 结果 |
|---|---|
| Qt 6.11.1 / MinGW 13.1 Release 配置、编译 | native exit 0 |
| 完整 CTest（含本机私有 golden） | 16 组中 15 组通过；1 组性能门限失败 |
| 失败明细 | `test_analysis` 的 640×360 软件 SRP 网格 CPU 测试：109 ms > 100 ms，独立复测 110 ms；其余 70 项通过。未修改原门限 |
| 降噪与协议回归 | AUD3、AUW1、AUF1、配套 UI、协议、状态等通过 |
| 连续流压力 | wallclock 12×10 s、旧契约、主窗口负载三组通过 |
| 不带私有录音的公开配置 | 三组降噪测试通过，明确 SKIP 14 个私有 golden 数据测项 |
| Python mock 测试 | 45 项通过；服务器 self-test 通过 |
| 配套 Windows 包 | 无外部 Qt/MinGW PATH 的启动、实际包内插件加载通过 |

结果见 `publication_tests.json` 与 `test_logs/`。测试日志的本机源码/构建路径替换为占位符，原音频不随代码公开。冻结 R2 的此前验证记录经路径脱敏转录到 `frozen_r2_validation.json`；其旧状态为“板上复测待完成”，之后用户在会话中反馈停播问题已解决，二者时间与证据来源不同。

`ctest` 不应被描述为“全部通过”：本次保留了软件热力图 CPU 门限失败。这不等于降噪处理或连续流测试失败，也不代表任意主机都达到该性能预算。

## FPGA 与数值证据范围

`physical_summary.json` 与 `routed_reports/` 对应最终冻结整板 Stage3 实现。报告只替换了本地工作路径。整板 100 MHz setup/hold 均 +0.006 ns，资源占用非常高；必须与本次软件调度验证分开解读。本次整理未重新布线或在新环境冷重建整板。

真实 HLS/vendor 数值测试、非零后处理测试及 raw48 拓扑启动零数据 smoke 是不同层级，不能合称一次覆盖全部输入的 raw48→扬声器非零整链板上验收。软件压力测试检查的是传输、排队和输出样本，不是客观声学降噪效果。LogMMSE 数值域及浮点转换限制见对应 JSON。

## 身份校验

- `release_manifest.json`：发布 ZIP 及包内 exe、bit、运行库和示例配置的 SHA256。
- `source_provenance.json`：复制时的冻结源身份；个别文档/测试路径/构建脚本随后有公开打包适配，并不声称这些适配文件仍保持原 SHA。
- `published_source_manifest.json`：提交前的最终公开源文件 SHA256（不包含自身和校验报告，Git commit 再绑定整棵树）。
- `.bit` 和 `.dcp` 作为 Release 附件发布，不放入 Git 历史；私有录音、标定和个人设置不发布。
