# WGC_Capture

WGC_Capture 是一个 Qt Widgets 桌面程序，用 Windows Graphics Capture（WGC）采集单个窗口或整台显示器，并在应用内实时预览。预览保持原始宽高比，界面下方显示采集尺寸和当前状态。

## 构建与运行

1. 在 Windows 上准备 Qt 6 的 MSVC x64 套件、支持 C++20 的 MSVC 编译器，以及包含 C++/WinRT 和 Windows Graphics Capture 互操作头文件的 Windows SDK。
2. 用 Qt Creator 打开 `WGCWidget.pro`，选择对应的 MSVC x64 Kit，将构建目录设在源码目录之外或 `build/` 下。
3. 构建并运行目标 `WGC_Capture`。程序使用 D3D11 硬件设备，运行环境需支持通过窗口或显示器句柄创建 WGC 采集项。

## 采集操作

1. 在下拉列表中选择显示器，或选择其他进程中可见、有标题且未最小化的顶层窗口。刚打开的窗口可通过“刷新列表”加入。
2. 点击“开始采集”。调整目标窗口大小时，界面会更新采集尺寸。调整预览窗口大小时，画面会居中缩放并以黑色填充留白。
3. 切换目标时，先在列表中选中新目标，再点击“切换 / 重新采集”。切换会先结束当前会话。如果新目标已关闭或最小化，可刷新列表后重选。
4. 点击“停止采集”可清空预览。采集目标关闭后，程序也会停止当前会话。

## 源码与采集流程

| 文件 | 职责 |
| --- | --- |
| `main.cpp` | 在 Qt 主线程初始化 WinRT 单线程单元，根据启动参数进入主窗口或自测。 |
| `MainWindow.h/.cpp` | 枚举显示器和窗口，保存目标类型与原生句柄，通过信号更新按钮、尺寸和状态文本。 |
| `WGCWidget.h/.cpp` | 管理 WGC 会话和帧池，复制 D3D11 纹理，使用 Direct2D 和 DXGI 交换链绘制预览。 |
| `tests/SmokeTest.h/.cpp` | 创建动态测试窗口，驱动真实采集会话并检查渲染结果。 |
| `WGCWidget.pro` | qmake 项目配置，声明 Qt 模块、编译选项及 Windows 图形库依赖。 |

`WGCWidget` 通过 `startCaptureWindow(HWND)` 或 `startCaptureMonitor(HMONITOR)` 创建采集项，并为其建立双缓冲 BGRA 帧池。Qt 定时器在 UI 线程调用 `processFrame()`，每次最多取一帧。目标内容尺寸改变时，控件先归还该帧，再按新尺寸重建帧池。

正常预览时，`copyFrame()` 通过 D3D11 的 `CopyResource` 把帧复制到控件持有的 GPU 纹理。这样可以及时归还 WGC 帧，并在后续的 `paintEvent()` 中用 Direct2D 将纹理绘制到交换链后缓冲。取帧与绘制共用 UI 线程，耗时的界面操作会影响预览更新。

## 运行集成自测

在有交互式桌面的 Windows 会话中，通过 Qt Creator 的运行参数传入 `--self-test`，或在已配置 Qt 运行环境的 PowerShell 中从可执行文件目录运行：

```powershell
$test = Start-Process .\WGC_Capture.exe -ArgumentList '--self-test' -Wait -PassThru
$test.ExitCode
Get-Content .\smoke-test.txt
```

自测会显示动态色块窗口和预览窗口，读取 GPU 后缓冲检查蓝色背景与黄色色块，并依次检查尺寸变化、手动停止、显示器采集、切回窗口、关闭目标和无效句柄。超时阈值为 30 秒。

报告 `smoke-test.txt` 和像素检查截图 `smoke-preview.png` 写在可执行文件所在目录。退出码 `0` 表示通过，`1` 表示测试或初始化失败，`2` 表示无法打开报告文件。
