#pragma once
#include <QWidget>
class QComboBox;
class QPushButton;
class QLabel;
class WGCWidget;

class MainWindow final : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
private:
    void refreshList();
    void startSelected();
    void updateButtons();
    QComboBox *m_targets;
    QPushButton *m_start;
    QPushButton *m_stop;
    QLabel *m_status;
    QLabel *m_size;
    WGCWidget *m_viewer;
};
