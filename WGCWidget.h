#pragma once
#include <QSize>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <memory>
#include <functional>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

class QApplication;

// 应用侧的取帧、复制和绘制全部在 Qt UI 线程执行。
class WGCWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit WGCWidget(QWidget *parent = nullptr);
    ~WGCWidget() override;
    bool initialize();
    bool startCaptureWindow(HWND hwnd);
    bool startCaptureMonitor(HMONITOR monitor);
    void stopCapture();
    bool isCapturing() const { return m_capturing; }
    QSize contentSize() const { return m_contentSize; }
    quint64 frameCount() const { return m_frameCount; }
    quint64 presentCount() const { return m_presentCount; }
    QString lastError() const { return m_lastError; }
    QPaintEngine *paintEngine() const override { return nullptr; }
signals:
    void statusChanged(const QString &message);
    void captureStateChanged(bool capturing);
    void errorOccurred(const QString &message);
    void frameSizeChanged(QSize size);
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
private:
    friend int runSmokeTest(QApplication &app);
    // 仅集成测试设置：EndDraw 后、Present 前检查真实后缓冲像素。
    std::function<void()> m_renderProbe;
    // 采集流水线：initD3D / initRenderer（设备与交换链）→ createCaptureItem（采集项）
    // → startSession（帧池 + 会话）→ processFrame（取帧）→ copyFrame（纹理拷贝）
    // → paintEvent（呈现）。创建与释放全都固定在同一个 UI 线程上。
    void initD3D();
    // 基于同一 D3D 设备建立 D2D 链路与绑定 winId() 的翻转模型交换链。
    void initRenderer();
    // 按客户区物理像素重建后缓冲与 D2D 目标位图；尺寸未变则为空操作。
    void resizeSwapChain();
    // 把 HWND 或 HMONITOR 转成 GraphicsCaptureItem 并启动会话；失败返回 false。
    bool createCaptureItem(HWND hwnd, HMONITOR monitor);
    // 依据采集项尺寸创建帧池与会话，并启动采集定时器。
    void startSession();
    // 采集节拍回调：TryGetNextFrame 取最新帧，拷贝后请求重绘。
    void processFrame();
    // 把 WGC 帧纹理 source 拷进自有纹理，content 是该帧的有效像素尺寸。
    void copyFrame(ID3D11Texture2D *source, QSize content);
    // 逆序释放整套 GPU 资源；必须在 stopCapture 之后调用。
    void releaseGraphics();
    // 统一错误出口：记录错误串并停止采集，graphicsFailure 时连带销毁 GPU 资源。
    void fail(const QString &operation, const winrt::hresult_error &error,
              bool graphicsFailure = false);
    // 采集节拍定时器：替代帧到达事件，使取帧在 UI 线程串行执行。
    QTimer m_captureTimer;
    // m_initialized：D3D / D2D / 交换链是否就绪；m_capturing：是否存在活动会话。
    bool m_initialized = false;
    bool m_capturing = false;
    // 仅窗口目标非空，供 processFrame 轮询 IsWindow 判断目标是否被销毁。
    HWND m_targetWindow = nullptr;
    QString m_lastError;
    // m_framePoolSize：当前帧池尺寸；m_contentSize：帧内有效内容尺寸；
    // m_backBufferSize：交换链后缓冲的物理像素尺寸。三者可以互不相同。
    QSize m_framePoolSize, m_contentSize, m_backBufferSize;
    quint64 m_frameCount = 0, m_presentCount = 0;
    // 以下 COM / WinRT 资源存在引用依赖，必须自下而上逆序释放：
    // 交换链与 D2D 位图 → D2D 设备 → D3D 设备上下文 → D3D 设备。
    // D3D 设备是所有资源的根，必须最后释放（见 releaseGraphics）。
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1Device> m_d2dDevice;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dContext;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_d2dTargetBitmap;
    // m_latestTexture / m_captureBitmap：CopyResource 的目标，归本进程所有，
    // 与 WGC 帧纹理完成解耦，帧 Close 之后依然可以拿来绘制。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_latestTexture;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_captureBitmap;
    // WGC 三件套 + D3D 设备的 WinRT 包装。它们内部持有底层 D3D 设备的引用，
    // 必须先 Close 再置空，且要在 D3D 设备释放之前完成。
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{nullptr};
    // m_targetClosed：Closed 回调写入的跨对象标记（回调刻意不捕获 this）；
    // m_closedToken / m_closedSubscribed：订阅凭据，必须在换目标前撤销。
    std::shared_ptr<std::atomic_bool> m_targetClosed;
    winrt::event_token m_closedToken{};
    bool m_closedSubscribed = false;
};
