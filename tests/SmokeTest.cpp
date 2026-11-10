#include "SmokeTest.h"
#include "../WGCWidget.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QScreen>
#include <QTextStream>
#include <QTimer>
#include <memory>
#include <cstring>

namespace {
class TestTarget final : public QWidget {
public:
    int tick = 0;
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(25, 80, 160));
        p.fillRect(QRect(20 + tick % 160, 60, 100, 100), QColor(240, 180, 40));
        p.setPen(Qt::white);
        p.drawText(20, 30, QStringLiteral("WGC live test — %1").arg(tick));
    }
};
}

// 在真实桌面运行真实 WGC/D3D 路径；不依赖鼠标操作。
int runSmokeTest(QApplication &app)
{
    app.setQuitOnLastWindowClosed(false);
    QFile report(QDir(app.applicationDirPath()).filePath(QStringLiteral("smoke-test.txt")));
    if (!report.open(QIODevice::WriteOnly | QIODevice::Text)) return 2;
    QTextStream log(&report);
    auto write = [&](const QString &text) { log << text << '\n'; log.flush(); };
    WGCWidget viewer;
    viewer.setWindowTitle(QStringLiteral("WGC smoke test preview"));
    viewer.setGeometry(680, 100, 600, 400);
    viewer.show();
    auto target = std::make_unique<TestTarget>();
    target->setWindowTitle(QStringLiteral("WGC smoke test source"));
    target->setGeometry(80, 100, 440, 320);
    target->show();
    // 测试启动器隐藏进程的初始窗口；采集与呈现测试必须明确显示这两个测试窗口。
    ShowWindow(reinterpret_cast<HWND>(viewer.winId()), SW_SHOWNOACTIVATE);
    ShowWindow(reinterpret_cast<HWND>(target->winId()), SW_SHOWNOACTIVATE);
    QTimer animation, poll, watchdog;
    int stage = 0;
    bool done = false, expectedError = false, pixelsVerified = false;
    QSize original;
    quint64 previousPresents = 0;
    auto finish = [&](bool ok, const QString &reason) {
        if (done) return;
        done = true;
        poll.stop(); animation.stop(); watchdog.stop();
        viewer.stopCapture();
        write((ok ? QStringLiteral("PASS: ") : QStringLiteral("FAIL: ")) + reason);
        app.exit(ok ? 0 : 1);
    };
    viewer.m_renderProbe = [&] {
        if (done || pixelsVerified || viewer.frameCount() < 3) return;
        try {
            // GDI 抓窗可能无法读出 flip-model 交换链，直接验证 GPU 后缓冲。
            Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer, staging;
            winrt::check_hresult(viewer.m_swapChain->GetBuffer(
                0, IID_PPV_ARGS(buffer.GetAddressOf())));
            D3D11_TEXTURE2D_DESC desc{};
            buffer->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            winrt::check_hresult(viewer.m_d3dDevice->CreateTexture2D(
                &desc, nullptr, staging.GetAddressOf()));
            viewer.m_d3dContext->CopyResource(staging.Get(), buffer.Get());
            QImage screenshot(int(desc.Width), int(desc.Height), QImage::Format_ARGB32);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(viewer.m_d3dContext->Map(
                staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            for (int y = 0; y < screenshot.height(); ++y) {
                std::memcpy(screenshot.scanLine(y),
                    static_cast<const char *>(mapped.pData) + size_t(y) * mapped.RowPitch,
                    size_t(screenshot.width()) * 4);
            }
            viewer.m_d3dContext->Unmap(staging.Get(), 0);
            // 交换链 alpha 被忽略，导出时显式设为不透明。
            int blue = 0, yellow = 0;
            for (int y = 0; y < screenshot.height(); ++y) {
                auto *row = reinterpret_cast<QRgb *>(screenshot.scanLine(y));
                for (int x = 0; x < screenshot.width(); ++x) {
                    row[x] |= 0xff000000;
                    const QColor color(row[x]);
                    if (color.blue() > 120 && color.green() > 45 &&
                        color.green() < 120 && color.red() < 70) ++blue;
                    if (color.red() > 190 && color.green() > 120 && color.blue() < 100) ++yellow;
                }
            }
            screenshot.save(QDir(app.applicationDirPath()).filePath(QStringLiteral("smoke-preview.png")));
            const int total = screenshot.width() * screenshot.height();
            if (blue < total / 10 || yellow < total / 200) {
                finish(false, QStringLiteral("rendered pixel check failed: blue=%1 yellow=%2")
                    .arg(blue).arg(yellow));
                return;
            }
            pixelsVerified = true;
            write(QStringLiteral("GPU back-buffer pixels match blue/yellow test pattern: OK"));
        } catch (const winrt::hresult_error &error) {
            finish(false, QString::fromWCharArray(error.message().c_str()));
        }
    };
    QObject::connect(&viewer, &WGCWidget::errorOccurred, &app, [&](const QString &message) {
        if (!expectedError) finish(false, message);
    });
    QObject::connect(&animation, &QTimer::timeout, &app, [&] {
        if (target) { ++target->tick; target->update(); }
    });
    animation.start(70);
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &app, [&] {
        finish(false, QStringLiteral("timeout at stage %1, frames=%2, presents=%3, visible=%4")
            .arg(stage).arg(viewer.frameCount()).arg(viewer.presentCount())
            .arg(IsWindowVisible(reinterpret_cast<HWND>(viewer.winId()))));
    });
    watchdog.start(30000);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (done) return;
        switch (stage) {
        case 0:
            if (!viewer.initialize()) { finish(false, viewer.lastError()); return; }
            if (!viewer.startCaptureWindow(reinterpret_cast<HWND>(target->winId()))) {
                finish(false, viewer.lastError()); return;
            }
            previousPresents = viewer.presentCount();
            write(QStringLiteral("capture session started; waiting for frames"));
            stage = 1;
            break;
        case 1:
            if (!pixelsVerified || viewer.frameCount() < 3 || viewer.presentCount() <= previousPresents) return;
            write(QStringLiteral("window capture + D2D presentation: OK"));
            original = viewer.contentSize();
            previousPresents = viewer.presentCount();
            target->resize(580, 390);
            viewer.resize(700, 460);
            stage = 2;
            break;
        case 2:
            if (viewer.contentSize().width() <= original.width() ||
                viewer.contentSize().height() <= original.height() ||
                viewer.presentCount() <= previousPresents) return;
            write(QStringLiteral("target resize + frame-pool recreate + preview resize: OK"));
            viewer.stopCapture();
            if (viewer.isCapturing() || !viewer.contentSize().isEmpty()) {
                finish(false, QStringLiteral("stop did not clear state")); return;
            }
            write(QStringLiteral("manual stop clears state: OK"));
            if (!viewer.startCaptureMonitor(MonitorFromWindow(
                    reinterpret_cast<HWND>(viewer.winId()), MONITOR_DEFAULTTOPRIMARY))) {
                finish(false, viewer.lastError()); return;
            }
            stage = 3;
            break;
        case 3:
            if (viewer.frameCount() < 3) return;
            write(QStringLiteral("monitor capture after window capture: OK"));
            if (!viewer.startCaptureWindow(reinterpret_cast<HWND>(target->winId()))) {
                finish(false, viewer.lastError()); return;
            }
            stage = 4;
            break;
        case 4:
            if (viewer.frameCount() < 3) return;
            write(QStringLiteral("switch back / restart: OK"));
            target.reset();
            stage = 5;
            break;
        case 5:
            if (viewer.isCapturing()) return;
            write(QStringLiteral("closed target stops capture: OK"));
            expectedError = true;
            if (viewer.startCaptureWindow(nullptr) || viewer.isCapturing()) {
                finish(false, QStringLiteral("invalid target accepted")); return;
            }
            expectedError = false;
            write(QStringLiteral("invalid target rejected: OK"));
            finish(true, QStringLiteral("all real WGC smoke checks completed"));
            break;
        }
    });
    poll.start(150);
    const int result = app.exec();
    poll.stop(); animation.stop(); watchdog.stop();
    viewer.m_renderProbe = {};
    // log/report 比 viewer 更晚析构；避免退出时触发仍引用局部变量的 lambda。
    QObject::disconnect(&viewer, nullptr, &app, nullptr);
    return result;
}
