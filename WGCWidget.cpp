#include "WGCWidget.h"
#include <QDebug>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <algorithm>
#include <d2d1_1helper.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;
namespace WGD11 = winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;
using winrt::check_hresult;

WGCWidget::WGCWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(320, 180);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    // 采集节拍用 QTimer 驱动，而不是订阅帧池的 FrameArrived 事件：回调落在 UI
    // 线程，天然与 paintEvent 串行，无需加锁即可访问非线程安全的 D3D11 立即
    // 上下文与 D2D 设备上下文。
    // 代价是 UI 线程被阻塞时节拍会被拖慢，而 TryGetNextFrame 只返回最新的一帧，
    // 中间的帧会被丢弃 —— 若要不丢帧需改用 FrameArrived + 后台队列。
    m_captureTimer.setTimerType(Qt::PreciseTimer);
    m_captureTimer.setInterval(16);
    connect(&m_captureTimer, &QTimer::timeout, this, &WGCWidget::processFrame);
}

// 析构顺序不可颠倒：先停采集释放全部 WinRT 对象，再销毁 D3D 设备。
// 帧池 / 会话 / IDirect3DDevice 内部都持有 D3D 设备的引用，若设备先被释放，
// 引用计数会残留（D3D11 调试层会报 live object 泄漏）。
// 另外本对象必须在 main() 里的 WinRT apartment 之前销毁完毕。
WGCWidget::~WGCWidget()
{
    // 父窗口正在销毁，不再通知它操作其他子控件。
    const QSignalBlocker blocker(this);
    stopCapture();
    releaseGraphics();
}

// 幂等：图形资源就绪后直接返回 true；失败时错误信息已由 fail() 记录。
// 只建立设备与交换链，不涉及任何 WGC 对象 —— 切换采集目标不会重建图形设备。
// createCaptureItem 内部会兜底调用一次，因此也可以由上层在显示前显式调用。
bool WGCWidget::initialize()
{
    if (m_initialized) return true;
    try {
        initD3D();
        initRenderer();
        m_initialized = true;
        m_lastError.clear();
        emit statusChanged(QStringLiteral("图形设备已就绪，请选择目标并开始采集"));
        update();
        return true;
    } catch (const winrt::hresult_error &error) {
        fail(QStringLiteral("初始化图形设备失败"), error, true);
        return false;
    }
}

// 创建 D3D11 硬件设备与立即上下文，它是整条 WGC / D2D 链路的资源根。
// D3D11_CREATE_DEVICE_BGRA_SUPPORT 是必需的：D2D 依赖 BGRA 格式通过 DXGI 互操作
// 绘制，缺少该标志时后续的 CreateBitmapFromDxgiSurface 会直接失败。
// 该设备稍后会被 CreateDirect3D11DeviceFromDXGIDevice 包装后交给 WGC 使用，
// 因此它的生命周期必须覆盖整个采集过程。
// 未开启调试层，也未请求同步接口：立即上下文只能在本线程使用。
void WGCWidget::initD3D()
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, m_d3dDevice.GetAddressOf(), nullptr,
        m_d3dContext.GetAddressOf()));
}

// 基于同一个 D3D11 设备建立 D2D 渲染链路，并创建绑定本窗口句柄的 DXGI 交换链。
// D2D1_FACTORY_TYPE_SINGLE_THREADED 与「全部逻辑都在 UI 线程」的模型一致：
// 若要在其它线程绘制，必须改用 MULTI_THREADED 工厂并自行加锁。
// 注意 D2D 设备与 WGC 帧池共用同一个 D3D 设备，这样帧纹理才能被直接拷贝与绘制。
void WGCWidget::initRenderer()
{
    ComPtr<IDXGIDevice> dxgi;
    check_hresult(m_d3dDevice.As(&dxgi));
    D2D1_FACTORY_OPTIONS options{};
    check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &options,
        reinterpret_cast<void **>(m_d2dFactory.GetAddressOf())));
    check_hresult(m_d2dFactory->CreateDevice(dxgi.Get(), m_d2dDevice.GetAddressOf()));
    check_hresult(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        m_d2dContext.GetAddressOf()));

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    check_hresult(dxgi->GetAdapter(adapter.GetAddressOf()));
    check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    // 翻转模型 FLIP_DISCARD 的硬性约束：BufferCount >= 2、SampleDesc.Count == 1，
    // 且每次 ResizeBuffers 后旧缓冲内容被丢弃（必须重画一整帧，不能增量更新）。
    // ALPHA_MODE_IGNORE 让窗口始终不透明，避免 DWM 参与 alpha 合成。
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    const auto hwnd = reinterpret_cast<HWND>(winId());
    check_hresult(factory->CreateSwapChainForHwnd(m_d3dDevice.Get(), hwnd,
        &desc, nullptr, nullptr, m_swapChain.GetAddressOf()));
    // 屏蔽 Alt+Enter：由 DXGI 自行切全屏会让这里缓存的后缓冲与目标位图立刻失效。
    check_hresult(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
    // 立刻按当前客户区建立首块后缓冲；窗口尺寸为 0 时会跳过，留到首次绘制时补建。
    resizeSwapChain();
}

// 按客户区当前物理像素重建后缓冲与 D2D 目标位图。
// 由 paintEvent 调用（而非 resizeEvent），以保证 GUI 尺寸变化与设备重建在同一线程。
void WGCWidget::resizeSwapChain()
{
    if (!m_swapChain || !m_d2dContext) return;
    // 原生客户区是物理像素，兼容跨屏 DPI 变化。
    RECT rect{};
    if (!GetClientRect(reinterpret_cast<HWND>(winId()), &rect)) winrt::throw_last_error();
    const QSize size(rect.right - rect.left, rect.bottom - rect.top);
    if (size.isEmpty() || (size == m_backBufferSize && m_d2dTargetBitmap)) return;
    // ResizeBuffers 之前必须解开 D2D 对旧后缓冲的所有引用（先解 Target 再释放位图）：
    // 只要还残留一个引用，ResizeBuffers 就会返回 DXGI_ERROR_INVALID_CALL。
    m_d2dContext->SetTarget(nullptr);
    m_d2dTargetBitmap.Reset();
    // DXGI_FORMAT_UNKNOWN 表示沿用创建时的格式；设备丢失时这里会返回
    // DXGI_ERROR_DEVICE_REMOVED，由调用方的 try 捕获并走 fail(..., true) 重建链路。
    check_hresult(m_swapChain->ResizeBuffers(2, UINT(size.width()), UINT(size.height()),
        DXGI_FORMAT_UNKNOWN, 0));
    ComPtr<IDXGISurface> buffer;
    check_hresult(m_swapChain->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf())));
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    check_hresult(m_d2dContext->CreateBitmapFromDxgiSurface(buffer.Get(), &props,
        m_d2dTargetBitmap.GetAddressOf()));
    m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
    m_backBufferSize = size;
}

bool WGCWidget::startCaptureWindow(HWND hwnd)
{
    return createCaptureItem(hwnd, nullptr);
}

bool WGCWidget::startCaptureMonitor(HMONITOR monitor)
{
    return createCaptureItem(nullptr, monitor);
}

// hwnd 与 monitor 二者互斥：hwnd 非空按窗口采集，否则按显示器采集。
// 成功返回 true（会话此时已启动）；失败返回 false 并通过 errorOccurred 上报原因。
// 本函数是可重入的：重复调用等价于「切换采集目标」。
bool WGCWidget::createCaptureItem(HWND hwnd, HMONITOR monitor)
{
    // 先彻底停掉上一次采集：帧池的像素尺寸与会话都绑定旧目标，无法复用。
    stopCapture();
    // 图形设备允许延后到首次采集时才建立，这里做一次兜底初始化。
    if (!initialize()) return false;
    try {
        // WGC 需要 Windows 10 1803 以上的桌面会话；部分远程桌面 / 无 GPU 环境
        // 会返回 false，提前检查可以给出更明确的提示。
        if (!WGC::GraphicsCaptureSession::IsSupported()) {
            throw winrt::hresult_error(E_NOTIMPL, L"当前系统不支持 Windows Graphics Capture");
        }
        // 最小化窗口的采集会话不会产出有效内容（尺寸/画面都不可靠），
        // 句柄已失效同理 —— 与其让用户看黑屏，不如直接拒绝。
        if (hwnd) {
            if (!IsWindow(hwnd) || IsIconic(hwnd)) {
                throw winrt::hresult_error(E_INVALIDARG, L"目标窗口已关闭或最小化，请恢复窗口并刷新列表");
            }
        } else {
            MONITORINFO info{sizeof(info)};
            if (!monitor || !GetMonitorInfoW(monitor, &info)) {
                throw winrt::hresult_error(E_INVALIDARG, L"显示器句柄无效，请刷新列表");
            }
        }
        // GraphicsCaptureItem 只能通过 Interop 工厂从原生句柄创建，没有公开的
        // 构造函数；put_abi 提供的是对象指针的写入位置，成功后由 m_item 持有引用。
        auto factory = winrt::get_activation_factory<WGC::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        if (hwnd) {
            check_hresult(factory->CreateForWindow(hwnd,
                winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(m_item)));
        } else {
            check_hresult(factory->CreateForMonitor(monitor,
                winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(m_item)));
        }
        // 记住窗口句柄：显示器目标没有句柄，靠 item 的 Closed 事件感知；
        // 窗口目标则额外用 IsWindow 轮询兜底（Closed 事件偶有延迟）。
        m_targetWindow = hwnd;
        // item 创建成功不等于会话成功：startSession 仍可能抛出（尺寸非法、
        // 设备丢失、目标拒绝等），统一由下面的 catch 走 fail() 处理 ——
        // 这类失败只停采集、不销毁图形设备，便于用户直接重试。
        startSession();
        return true;
    } catch (const winrt::hresult_error &error) {
        fail(QStringLiteral("启动采集失败"), error);
        return false;
    }
}

// 依据采集项尺寸创建帧池与会话，并启动采集定时器。
// 前置条件：m_item 有效、D3D 设备已就绪；所有失败都以 winrt::hresult_error 抛出，
// 由调用方（createCaptureItem）统一处理。
void WGCWidget::startSession()
{
    // Size() 是内容的原始像素尺寸，也是帧池缓冲区分配的依据，为 0 必须拒绝。
    // 注意它还受进程 DPI 感知级别影响：未声明 Per-Monitor V2 感知时，高分屏上
    // 采集到的是 DWM 缩放后的位图（画面会偏模糊），需在应用清单里声明 DPI 感知。
    const auto size = m_item.Size();
    if (size.Width <= 0 || size.Height <= 0) {
        throw winrt::hresult_error(E_INVALIDARG, L"目标尺寸无效");
    }
    // 把 COM 的 D3D11 设备包装成 WinRT 的 IDirect3DDevice 交给 WGC 使用。
    // 包装对象内部会增加设备引用计数，因此必须在 D3D 设备之前释放。
    ComPtr<IDXGIDevice> dxgi;
    check_hresult(m_d3dDevice.As(&dxgi));
    winrt::com_ptr<::IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
    m_winrtDevice = inspectable.as<WGD11::IDirect3DDevice>();
    // 创建抢占式（free-threaded）帧池：它允许从任意线程取帧，不绑定创建它的
    // apartment；这里虽然只用 UI 线程，但可以避免 STA 亲和性带来的限制。
    // 参数 2 = 帧缓冲数量：取小值降低延迟，但处理跟不上时会丢帧。
    // 格式选 B8G8R8A8 是为了与交换链一致、绘制时无需再转换；
    // 若需要 HDR 应换成 R16G16B16A16Float 并自行处理色彩空间/色调映射。
    m_framePool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(m_winrtDevice,
        WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    m_framePoolSize = QSize(size.Width, size.Height);

    // “采集目标销毁”事件：窗口关闭或显示器断开时触发。
    // 回调不捕获 this；切换目标时旧回调只会更新旧标记。
    m_targetClosed = std::make_shared<std::atomic_bool>(false);
    m_closedToken = m_item.Closed([flag = m_targetClosed](auto const &, auto const &) {
        flag->store(true);
    });
    m_closedSubscribed = true;
    // 会话建立在帧池之上，并绑定采集项。会话属性（如支持的版本上采集鼠标指针的
    // IsCursorCaptureEnabled）必须在 StartCapture 之前设置；本项目保持默认：
    // 不采集指针、不绘制边框。
    m_session = m_framePool.CreateCaptureSession(m_item);
    // 调用之后帧才开始入池。定时器刻意放在最后启动，
    // 保证 processFrame 不会早于「帧池已就绪」被触发。
    m_session.StartCapture();
    m_capturing = true;
    m_lastError.clear();
    m_captureTimer.start();
    emit captureStateChanged(true);
    emit statusChanged(QStringLiteral("正在采集：%1").arg(
        QString::fromWCharArray(m_item.DisplayName().c_str())));
}

// 一个采集节拍：取一帧 → 拷进自有纹理 → 请求重绘。
// 每次调用最多处理一帧，没有新帧就立刻返回，既不等待也不做 CPU 回读。
void WGCWidget::processFrame()
{
    if (!m_capturing) return;
    // 目标存活检查：Closed 事件标记 + IsWindow 轮询双保险。目标消失后必须尽快
    // 停会话，否则 TryGetNextFrame 会持续报错，白白消耗 GPU 资源。
    if ((m_targetClosed && m_targetClosed->load()) ||
        (m_targetWindow && !IsWindow(m_targetWindow))) {
        stopCapture();
        emit statusChanged(QStringLiteral("采集目标已关闭，请刷新列表选择新目标"));
        return;
    }
    try {
        // 取「最新的一帧」：帧池按缓冲数去重，被跳过的中间帧不会积压。
        // 取到的 frame 必须显式 Close()，否则缓冲区被占满后这里会一直返回空。
        auto frame = m_framePool.TryGetNextFrame();
        if (!frame) return;
        // ContentSize 是这一帧中真正有效的像素区域，可能小于纹理尺寸，
        // 也可能随目标窗口缩放、分辨率或 DPI 变化而改变。
        const auto content = frame.ContentSize();
        if (content.Width <= 0 || content.Height <= 0) {
            frame.Close();
            return;
        }
        // 有效尺寸与当前帧池尺寸不一致时必须 Recreate，否则后续帧会被拉伸或裁切。
        // Recreate 要求手上没有任何未归还的帧，所以先 Close 再 Recreate；
        // 本节拍直接返回并丢弃这一帧，下一拍才会拿到新尺寸的帧。
        const QSize newSize(content.Width, content.Height);
        if (newSize != m_framePoolSize) {
            frame.Close();
            frame = nullptr;
            m_framePool.Recreate(m_winrtDevice,
                WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
            m_framePoolSize = newSize;
            return;
        }
        // 用局部作用域把 source 限制在 frame.Close() 之前：
        // 它是 WGC 帧纹理的 COM 接口指针，所有权归帧，一旦 Close 就可能被立刻
        // 复用，任何保留都会拿到错乱画面 —— 所以只能靠 copyFrame 拷走像素。
        {
            // Surface() 是 WinRT 对象，需经 IDirect3DDxgiInterfaceAccess 取出原生
            // COM 接口；设备丢失时这一步会直接返回 DXGI_ERROR_DEVICE_REMOVED。
            auto access = frame.Surface().as<
                ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> source;
            check_hresult(access->GetInterface(IID_PPV_ARGS(source.GetAddressOf())));
            // 这一句结束后，本帧的像素就已经落到自有纹理里了，
            // 之后的 Close 与重绘都与这份 WGC 帧无关。
            copyFrame(source.Get(), newSize);
        }
        frame.Close();
        ++m_frameCount;
        // 请求 Qt 重绘；真正用 D2D 把内容画进交换链发生在随后的 paintEvent。
        update();
    } catch (const winrt::hresult_error &error) {
        // 常见失败：设备丢失（DXGI_ERROR_DEVICE_REMOVED / ACCESS_LOST）、帧池已被
        // 关闭、目标内容受保护（部分受 DRM/HDCP 保护的内容不允许捕获）。
        // 这些都按图形失败处理：停采集并重建整套 GPU 资源。
        fail(QStringLiteral("处理采集帧失败"), error, true);
    }
}

// 把 WGC 帧纹理 source 做 GPU → GPU 拷贝（CopyResource），全程不落 CPU，
// 因此没有 Map/Unmap 回读开销，也不会打断 GPU 管线。
// source：帧内纹理，调用期间必须保持有效（所在的 frame 尚未 Close）。
// content：该帧有效像素区域，用于换算预览尺寸，通常 ≤ 纹理实际尺寸。
// 仅当纹理尺寸或格式发生变化时，才重建自有目标纹理与其 D2D 位图。
void WGCWidget::copyFrame(ID3D11Texture2D *source, QSize content)
{
    // desc 是 WGC 帧纹理的描述：Usage / BindFlags / MiscFlags 都由 WGC 决定
    // （带共享相关标志），不能直接当作自有资源长期使用，这里只借用其中的
    // Width / Height / Format / SampleDesc 来创建兼容的自有纹理。
    D3D11_TEXTURE2D_DESC desc{}, old{};
    source->GetDesc(&desc);
    if (m_latestTexture) m_latestTexture->GetDesc(&old);
    if (!m_latestTexture || desc.Width != old.Width || desc.Height != old.Height ||
        desc.Format != old.Format) {
        m_captureBitmap.Reset();
        m_latestTexture.Reset();
        // 只比对 Width / Height / Format：这三个变了就必须重画一张位图；
        // 采样数等其余字段由 WGC 决定且保持不变。
        auto ownedDesc = desc;
        // 关键：必须覆盖 Usage / BindFlags / CPUAccessFlags / MiscFlags。
        // - MiscFlags 清零，避免继承 WGC 的共享句柄 / 键控互斥语义；
        // - DEFAULT + CPUAccessFlags = 0 保证纹理常驻显存，可被 CopyResource 走硬件路径；
        // - BindFlags 同时保留 SHADER_RESOURCE 与 RENDER_TARGET，后者是
        //   CreateBitmapFromDxgiSurface 的硬性要求，缺了会直接失败。
        ownedDesc.Usage = D3D11_USAGE_DEFAULT;
        ownedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ownedDesc.CPUAccessFlags = 0;
        ownedDesc.MiscFlags = 0;
        check_hresult(m_d3dDevice->CreateTexture2D(&ownedDesc, nullptr,
            m_latestTexture.GetAddressOf()));
        ComPtr<IDXGISurface> surface;
        check_hresult(m_latestTexture.As(&surface));
        // D2D 规定：基于 DXGI 表面的位图，alpha 模式只能是 PREMULTIPLIED 或 IGNORE。
        // 采集到的画面 alpha 常常是 0，用 PREMULTIPLIED 会得到整幅透明/偏暗的结果，
        // 因此取 IGNORE，先把内容实心呈现出来。DPI 固定 96，不参与缩放计算。
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        check_hresult(m_d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &props,
            m_captureBitmap.GetAddressOf()));
    }
    // GPU → GPU；归还 WGC 帧后仍能绘制自己的图像。
    // CopyResource 要求源与目标尺寸、格式、采样数完全一致，这正是纹理尺寸一变
    // 就必须先重建目标纹理的原因；它一次性拷贝整份资源，无法只拷子区域。
    m_d3dContext->CopyResource(m_latestTexture.Get(), source);
    // 有效区域取 content 与纹理实际尺寸的较小者：内容尺寸有可能瞬时超出缓冲分配
    // 大小（切换显示器 / 改分辨率的那一瞬间），取 min 保证绘制区域始终落在纹理内。
    const QSize valid(std::min(content.width(), int(desc.Width)),
                      std::min(content.height(), int(desc.Height)));
    // 仅在尺寸真的变化时发信号，避免每帧都惊动上层 UI。
    if (m_contentSize != valid) {
        m_contentSize = valid;
        emit frameSizeChanged(valid);
    }
}

// Qt 不参与绘制（paintEngine 返回 nullptr 且窗口带 WA_PaintOnScreen），
// 画面完全由 D2D 直接画到交换链后缓冲，再 Present 交给 DWM 合成。
// 与 processFrame 同处 UI 线程，因此访问 m_captureBitmap 无需任何同步。
void WGCWidget::paintEvent(QPaintEvent *)
{
    // 图形设备还没建好时只能用 GDI 涂黑，否则窗口会露出上一次的残影。
    if (!m_initialized) {
        const auto hwnd = reinterpret_cast<HWND>(winId());
        RECT r{};
        GetClientRect(hwnd, &r);
        HDC dc = GetDC(hwnd);
        if (dc) {
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            ReleaseDC(hwnd, dc);
        }
        return;
    }
    try {
        resizeSwapChain();
        if (!m_d2dTargetBitmap) return;
        m_d2dContext->BeginDraw();
        m_d2dContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        // 按 contentSize 等比缩放并居中（letterbox），多余区域填黑边。
        if (m_captureBitmap && !m_contentSize.isEmpty()) {
            const auto target = m_d2dContext->GetSize();
            const float w = float(m_contentSize.width()), h = float(m_contentSize.height());
            const float scale = std::min(target.width / w, target.height / h);
            const float x = (target.width - w * scale) / 2;
            const float y = (target.height - h * scale) / 2;
            const auto dst = D2D1::RectF(x, y, x + w * scale, y + h * scale);
            const auto src = D2D1::RectF(0, 0, w, h);
            m_d2dContext->DrawBitmap(m_captureBitmap.Get(), &dst, 1,
                D2D1_INTERPOLATION_MODE_LINEAR, &src, nullptr);
        }
        // EndDraw 在设备丢失时返回 D2DERR_RECREATE_TARGET，由下面的 catch
        // 走 fail(..., true) 重建整条渲染链路；opacity = 1、线性插值做 scale。
        check_hresult(m_d2dContext->EndDraw());
        if (m_renderProbe) m_renderProbe();
        // SyncInterval = 1：等到下一次垂直同步再提交，避免撕裂。窗口被完全遮挡时
        // 会返回 DXGI_STATUS_OCCLUDED —— 它是成功码，不代表失败，无需特殊处理。
        check_hresult(m_swapChain->Present(1, 0));
        ++m_presentCount;
    } catch (const winrt::hresult_error &error) {
        fail(QStringLiteral("显示失败，请重新开始采集以重建图形资源"), error, true);
    }
}

void WGCWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // ResizeBuffers 在 paintEvent 中执行，统一检查物理像素尺寸。
    update();
}

// 停止采集并清空所有与当前目标绑定的状态；幂等，可重复调用。
// 释放顺序敏感：先停定时器杜绝新的 processFrame，再依次撤销事件、关闭会话与帧池。
void WGCWidget::stopCapture()
{
    m_captureTimer.stop();
    const bool wasCapturing = m_capturing;
    m_capturing = false;
    // 先撤销 Closed 订阅：token 只对当前 item 有效，换了目标后就是悬空凭据。
    if (m_item && m_closedSubscribed) {
        try { m_item.Closed(m_closedToken); }
        catch (const winrt::hresult_error &) { qWarning() << "Cannot revoke Closed event"; }
    }
    m_closedSubscribed = false;
    m_closedToken = {};
    m_targetClosed.reset();
    // 会话与帧池必须逐个 Close：只关会话会让帧池继续占用 GPU 缓冲资源。
    // 顺序为先会话后帧池（推荐顺序），二者内部都引用着 IDirect3DDevice /
    // D3D 设备，必须手工置空才能在 D3D 设备释放前把引用计数归零。
    // 前提：调用时不应有未 Close 的帧 —— processFrame 每帧都 Close，且帧池内的
    // 纹理已在 copyFrame 中被拷走，本对象不持有任何帧池资源。
    // 这些 Close 在对象已失效时会抛 RO_E_CLOSED，因此逐个 try 并忽略。
    if (m_session) {
        try { m_session.Close(); } catch (const winrt::hresult_error &) { qWarning() << "Cannot close session"; }
        m_session = nullptr;
    }
    if (m_framePool) {
        try { m_framePool.Close(); } catch (const winrt::hresult_error &) { qWarning() << "Cannot close frame pool"; }
        m_framePool = nullptr;
    }
    m_item = nullptr;
    m_winrtDevice = nullptr;
    m_targetWindow = nullptr;
    // 帧池尺寸、自有拷贝纹理与位图都绑定上一个目标：这里主动释放，
    // 免得重启采集时靠尺寸不匹配被动重建，白白多占一份显存。
    m_framePoolSize = {};
    m_captureBitmap.Reset();
    m_latestTexture.Reset();
    // contentSize 归零并通知上层，让 UI 回到「尚无画面」。
    m_contentSize = {};
    m_frameCount = 0;
    update();
    // 只有确实曾处于采集态才发信号，避免重复「停止」刷出一堆状态提示。
    if (wasCapturing) {
        emit frameSizeChanged({});
        emit captureStateChanged(false);
        emit statusChanged(QStringLiteral("采集已停止"));
    }
}

// 逆序释放整条 GPU 资源链，必须在 stopCapture() 之后调用。
// 顺序不可颠倒：D2D 上下文/位图持有交换链后缓冲的引用，D2D 设备/工厂又持有 D3D
// 设备的引用，倒着放会让 ResizeBuffers / Present 失败，或触发 D3D11 live object 泄漏。
// 释放后 m_initialized 归 false，下一次 initialize() 会整套重建（设备丢失的唯一恢复手段）。
void WGCWidget::releaseGraphics()
{
    // 先解开 D2D 上下文对后缓冲的绑定，否则 m_swapChain 的 Reset 无法真正释放表面。
    if (m_d2dContext) m_d2dContext->SetTarget(nullptr);
    m_captureBitmap.Reset();
    m_latestTexture.Reset();
    m_d2dTargetBitmap.Reset();
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory.Reset();
    m_swapChain.Reset();
    // 释放上下文前清空管线上残留的绑定与未完成的命令，避免延迟引用造成的撕裂。
    if (m_d3dContext) { m_d3dContext->ClearState(); m_d3dContext->Flush(); }
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
    m_initialized = false;
    m_backBufferSize = {};
}

// 统一错误出口：把 HRESULT（转成 8 位十六进制）与 WinRT 描述拼成错误串写入
// m_lastError，然后按 graphicsFailure 决定处理范围 —— 只有设备/渲染链路损坏
// （失效设备句柄、丢失设备等）才连带销毁 GPU 资源；目标句柄失效这类普通失败
// 保留图形设备，方便用户直接换目标重试。
// 不重新抛出异常，调用方可以继续走正常 return。
// 注意 fail() 内部会调用 stopCapture()，而后者会发信号 —— 因此 fail() 属于
// 「已开始清理」的状态转换点，不应再被递归调用。
void WGCWidget::fail(const QString &operation, const winrt::hresult_error &error,
                     bool graphicsFailure)
{
    const HRESULT code = error.code();
    m_lastError = QStringLiteral("%1（0x%2）：%3").arg(operation,
        QString::number(quint32(code), 16).rightJustified(8, QLatin1Char('0')),
        QString::fromWCharArray(error.message().c_str()));
    stopCapture();
    if (graphicsFailure) releaseGraphics();
    qWarning().noquote() << m_lastError;
    emit errorOccurred(m_lastError);
    update();
}
