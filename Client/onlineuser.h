#ifndef ONLINEUSER_H
#define ONLINEUSER_H

#include <QWidget>
#include <qlistwidget.h>
#include "client.h"

// 在线用户子窗口：展示服务端回发的全部在线用户名，双击即发送添加好友请求。
// 列表项文本直接就是用户名（不再带序号），顶部 hintLabel 用于反馈选中/发送结果。
namespace Ui {
class OnlineUser;
}

// 在线用户列表页
class OnlineUser : public QWidget
{
    Q_OBJECT

public:
    explicit OnlineUser(QWidget *parent = nullptr);
    ~OnlineUser();
    //更新列表中的在线用户
    void updateOnlineUser(const QStringList& userNames);
    // REQ-UX-001 A1：外部（响应处理）更新顶部提示条，保证操作结果可见
    void setHint(const QString &text);
private slots:
    // 单击：选中并给出可见反馈
    void on_listWidget_itemClicked(QListWidgetItem *item);
    // 双击：确认后发送好友请求
    void on_listWidget_itemDoubleClicked(QListWidgetItem *item);

private:
    // 含 listWidget 与 hintLabel
    Ui::OnlineUser *ui;
};

#endif // ONLINEUSER_H
