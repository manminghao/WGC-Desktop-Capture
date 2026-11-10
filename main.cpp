#include "MainWindow.h"
#include "tests/SmokeTest.h"
#include <QApplication>
#include <winrt/base.h>
#include <windows.h>

// WGC 是 WinRT API，进程必须先初始化 apartment 才能创建 GraphicsCaptureItem /
// 帧池等对象。这里使用单线程 apartment，Qt 事件循环所在的 UI 线程即其主线程，
// 因此本项目的 WGC 对象一律在该线程上创建、使用与销毁，不要搬到临时线程。
// 必须比 QApplication 和所有窗口活得更久。
struct WinrtApartment {
    WinrtApartment() { winrt::init_apartment(winrt::apartment_type::single_threaded); }
    ~WinrtApartment() { winrt::uninit_apartment(); }
};

int main(int argc, char *argv[])
{
    try {
        WinrtApartment apartment;
        QApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("WGC 采集预览"));
        if (app.arguments().contains(QStringLiteral("--self-test"))) return runSmokeTest(app);
        MainWindow window;
        window.show();
        return app.exec();
    } catch (const winrt::hresult_error &error) {
        MessageBoxW(nullptr, error.message().c_str(), L"WGC 初始化失败", MB_OK | MB_ICONERROR);
        return 1;
    } catch (const std::exception &error) {
        MessageBoxA(nullptr, error.what(), "WGC error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
