# 演示全屏按页面路由设计

## 问题

主窗口右上角“演示全屏”按钮固定连接到
`MainWindow::togglePresentationMode()`。因此即使当前显示“降噪处理”
页面，点击按钮仍会打开音视频融合演示窗口。降噪页面已有独立的全屏
窗口，但只能通过页面内部按钮进入。

## 设计

保留两个现有全屏窗口及其布局，不移动或复用内部控件。新增一个主窗口
级分派入口：

- 当前页是“降噪处理”时，确保音视频融合演示窗口关闭，并打开
  `NoiseReductionPage` 的独立演示窗口。
- 当前页是“音视频融合”时，沿用现有
  `MainWindow::togglePresentationMode()` 行为。
- 其他页面维持现有行为，仍打开音视频融合演示窗口。

`NoiseReductionPage` 暴露具备业务含义的公开方法
`showPresentation()`，内部继续调用现有私有
`setPresentationMode(true)`，不向主窗口暴露降噪演示窗口的布局细节。
页面失活时现有 `setActive(false)` 仍负责关闭降噪全屏，保证两个演示
窗口互斥。

视频预览双击行为不变：音视频融合视频仍切换音视频融合全屏，降噪视频
仍切换降噪全屏。本次只修复右上角全局按钮。

## 验证

新增主窗口 UI 回归测试：

1. 在音视频融合页点击右上角按钮，只打开 `presentationWindow`。
2. 退出后切换到降噪处理页，点击同一按钮，只打开
   `noiseReductionPresentationWindow`。
3. 退出降噪全屏后两个窗口均隐藏。

