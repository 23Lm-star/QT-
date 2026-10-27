#ifndef RECONNECTDIALOG_H
#define RECONNECTDIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>

class QLabel;
class QPushButton;
class QTimer;
class QShowEvent;
class QCloseEvent;

// 与服务端连接断开时的提示弹窗：显示转圈动画 + "重新连接服务器"按钮。
// 点击按钮通过 reconnectClicked() 信号通知外部发起重连。纯代码实现，无 .ui。
class ReconnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ReconnectDialog(QWidget *parent = nullptr);

signals:
    void reconnectClicked();   // 用户点击"重新连接服务器"

protected:
    // 显示时启动转圈动画
    void showEvent(QShowEvent *event) override;
    // 关闭时停止定时器，避免隐藏后空转
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onReconnectBtnClicked();
    // 定时切换转圈帧
    void onSpinnerTick();

private:
    void startSpinner();
    void stopSpinner();

    // 转圈字符
    QLabel *m_spinnerLabel;
    // 断开提示文字
    QLabel *m_textLabel;
    // 触发 reconnectClicked()
    QPushButton *m_reconnectBtn;
    // 120ms 周期
    QTimer *m_spinnerTimer;
    // 当前帧下标
    int m_spinnerIdx;
    // 四帧转圈序列：竖线、斜杠、横线、反斜杠
    static const QStringList s_spinnerFrames;
};

#endif // RECONNECTDIALOG_H
