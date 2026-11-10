#include "MainWindow.h"
#include "WGCWidget.h"
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariant>

namespace {
constexpr int HandleRole = Qt::UserRole;
constexpr int MonitorRole = Qt::UserRole + 1;
void addTarget(QComboBox *combo, const QString &name, quintptr handle, bool monitor)
{
    combo->addItem(name, QVariant::fromValue(handle));
    combo->setItemData(combo->count() - 1, monitor, MonitorRole);
}
BOOL CALLBACK enumMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        addTarget(reinterpret_cast<QComboBox *>(data),
            QStringLiteral("[显示器] %1 · %2 × %3%4")
                .arg(QString::fromWCharArray(info.szDevice))
                .arg(info.rcMonitor.right - info.rcMonitor.left)
                .arg(info.rcMonitor.bottom - info.rcMonitor.top)
                .arg(info.dwFlags & MONITORINFOF_PRIMARY ? QStringLiteral(" · 主屏") : QString()),
            reinterpret_cast<quintptr>(monitor), true);
    }
    return TRUE;
}
BOOL CALLBACK enumWindow(HWND hwnd, LPARAM data)
{
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    wchar_t title[1024]{};
    const int count = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    if (count > 0) {
        addTarget(reinterpret_cast<QComboBox *>(data),
            QStringLiteral("[窗口] %1").arg(QString::fromWCharArray(title, count)),
            reinterpret_cast<quintptr>(hwnd), false);
    }
    return TRUE;
}
}

MainWindow::MainWindow(QWidget *parent) : QWidget(parent)
{
    setWindowTitle(QStringLiteral("WGC 窗口与屏幕采集"));
    resize(1100, 740);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("Windows Graphics Capture"), this);
    title->setStyleSheet(QStringLiteral("font-size: 21px; font-weight: 600;"));
    layout->addWidget(title);
    auto *bar = new QHBoxLayout;
    m_targets = new QComboBox(this);
    m_targets->setMinimumWidth(300);
    m_targets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_targets->setPlaceholderText(QStringLiteral("选择一个窗口或显示器"));
    auto *refresh = new QPushButton(QStringLiteral("刷新列表"), this);
    m_start = new QPushButton(QStringLiteral("开始采集"), this);
    m_stop = new QPushButton(QStringLiteral("停止采集"), this);
    bar->addWidget(m_targets, 1);
    bar->addWidget(refresh);
    bar->addWidget(m_start);
    bar->addWidget(m_stop);
    layout->addLayout(bar);
    m_viewer = new WGCWidget(this);
    layout->addWidget(m_viewer, 1);
    m_status = new QLabel(QStringLiteral("正在初始化图形设备…"), this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_size = new QLabel(QStringLiteral("尚无画面"), this);
    layout->addWidget(m_status);
    layout->addWidget(m_size);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshList);
    connect(m_start, &QPushButton::clicked, this, &MainWindow::startSelected);
    connect(m_stop, &QPushButton::clicked, m_viewer, &WGCWidget::stopCapture);
    connect(m_targets, &QComboBox::currentIndexChanged, this, &MainWindow::updateButtons);
    connect(m_viewer, &WGCWidget::captureStateChanged, this, [this] { updateButtons(); });
    connect(m_viewer, &WGCWidget::statusChanged, m_status, &QLabel::setText);
    connect(m_viewer, &WGCWidget::errorOccurred, m_status, &QLabel::setText);
    connect(m_viewer, &WGCWidget::frameSizeChanged, this, [this](QSize size) {
        m_size->setText(size.isEmpty() ? QStringLiteral("尚无画面") :
            QStringLiteral("采集尺寸：%1 × %2 · BGRA · 等比例预览").arg(size.width()).arg(size.height()));
    });
    refreshList();
    QTimer::singleShot(0, m_viewer, [this] { m_viewer->initialize(); });
}

void MainWindow::refreshList()
{
    const auto handle = m_targets->currentData(HandleRole);
    const auto monitor = m_targets->currentData(MonitorRole);
    const QSignalBlocker blocker(m_targets);
    m_targets->clear();
    const bool monitorsOk = EnumDisplayMonitors(nullptr, nullptr, enumMonitor,
        reinterpret_cast<LPARAM>(m_targets)) != FALSE;
    const bool windowsOk = EnumWindows(enumWindow, reinterpret_cast<LPARAM>(m_targets)) != FALSE;
    int selected = -1;
    for (int i = 0; i < m_targets->count(); ++i) {
        if (m_targets->itemData(i, HandleRole) == handle &&
            m_targets->itemData(i, MonitorRole) == monitor) { selected = i; break; }
    }
    m_targets->setCurrentIndex(selected);
    updateButtons();
    if (!monitorsOk || !windowsOk) {
        m_status->setText(QStringLiteral("部分目标枚举失败，请重试刷新；现有采集保持运行"));
    } else if (!m_viewer->isCapturing()) {
        m_status->setText(QStringLiteral("找到 %1 个候选目标，请选择后点击开始采集").arg(m_targets->count()));
    }
}

void MainWindow::startSelected()
{
    if (m_targets->currentIndex() < 0) return;
    const auto handle = m_targets->currentData(HandleRole).value<quintptr>();
    if (m_targets->currentData(MonitorRole).toBool())
        m_viewer->startCaptureMonitor(reinterpret_cast<HMONITOR>(handle));
    else
        m_viewer->startCaptureWindow(reinterpret_cast<HWND>(handle));
    updateButtons();
}

void MainWindow::updateButtons()
{
    m_start->setEnabled(m_targets->currentIndex() >= 0);
    m_start->setText(m_viewer->isCapturing() ? QStringLiteral("切换 / 重新采集") : QStringLiteral("开始采集"));
    m_stop->setEnabled(m_viewer->isCapturing());
}
