#ifndef CHAT_H
#define CHAT_H

#include <QWidget>

// 单聊窗口：展示与某个好友的会话消息，并负责发送聊天请求。
// 窗口自身不解析响应，消息由 Client/MsgHandler 收到后调用 updateShow_TE(s) 推入；
// m_strChatName 由 Friend 打开窗口时写入，既是窗口标题也是本条会话的对方用户名。
namespace Ui {
class Chat;
}

// 聊天页（纯展示 + 发送入口）
class Chat : public QWidget
{
    Q_OBJECT

public:
    QString m_strChatName;//聊天对象
    void updateShow_TE(QString Msg);//更新聊天框 - 一条信息
    void updateShow_TEs(QStringList Msgs);//更新聊天框 - 多条信息
    // 构造：建立 Designer 控件，初始不加载任何会话
    explicit Chat(QWidget *parent = nullptr);
    ~Chat();

private slots:
    void on_output_PB_clicked();//点击聊天消息发送按钮->发送消息

private:
    // Qt Designer 生成的控件集合
    Ui::Chat *ui;
};

#endif // CHAT_H
