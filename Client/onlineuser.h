#ifndef ONLINEUSER_H
#define ONLINEUSER_H

#include <QWidget>
#include <qlistwidget.h>
#include "client.h"

// 所有用户子窗口（REQ-PAGE-FETCH-001，原"在线用户"）：分页展示全部陌生用户
// （含离线、排除自己与好友），每项带在线状态；双击即发送添加好友请求。
// 列表项 Qt::UserRole 存纯用户名，显示文本附状态后缀；顶部 hintLabel 用于反馈。
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
    // 更新用户列表：append=false 整页替换（第 0 页/刷新），append=true 追加下一页
    void updateOnlineUser(const QVector<QPair<QString,bool>>& entries, uint total, bool hasMore, bool append);
    // 从第 0 页重新拉取（打开窗口/刷新入口）
    void resetAndRequest();
    // REQ-UX-001 A1：外部（响应处理）更新顶部提示条，保证操作结果可见
    void setHint(const QString &text);
private slots:
    // 加载更多：按已加载条数请求下一页
    void on_loadMore_PB_clicked();
    // 单击：选中并给出可见反馈
    void on_listWidget_itemClicked(QListWidgetItem *item);
    // 双击：确认后发送好友请求
    void on_listWidget_itemDoubleClicked(QListWidgetItem *item);

private:
    // 含 listWidget、hintLabel 与 loadMore_PB
    Ui::OnlineUser *ui;
    // REQ-PAGE-FETCH-001 分页状态
    uint m_offset=0;        // 已加载条数（下一页起点）
    uint m_total=0;         // 符合条件总人数
    bool m_hasMore=false;   // 还有下一页
};

#endif // ONLINEUSER_H
