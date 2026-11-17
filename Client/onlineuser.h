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
    // REQ-PAGE-FETCH-002：capped=1 表示结果总量超上限，提示条可见封顶说明
    void updateOnlineUser(const QVector<QPair<QString,bool>>& entries, uint total, bool hasMore, bool append, bool capped);
    // 从第 0 页重新拉取（打开窗口/刷新入口；清空搜索词）
    void resetAndRequest();
    // REQ-UX-001 A1：外部（响应处理）更新顶部提示条，保证操作结果可见
    void setHint(const QString &text);
private slots:
    // 加载更多：按已加载条数请求下一页（携带当前搜索词）
    void on_loadMore_PB_clicked();
    // REQ-PAGE-FETCH-002：搜索（按钮/回车同口径），更换/清空关键词从第 0 页重拉
    void on_search_PB_clicked();
    void on_search_LE_returnPressed();
    // 单击：选中并给出可见反馈
    void on_listWidget_itemClicked(QListWidgetItem *item);
    // 双击：确认后发送好友请求
    void on_listWidget_itemDoubleClicked(QListWidgetItem *item);

private:
    // 含 listWidget、hintLabel、loadMore_PB 与搜索行
    Ui::OnlineUser *ui;
    // REQ-PAGE-FETCH-002：按 limit=50 与当前搜索词请求指定偏移页
    void requestPage(uint offset);
    // REQ-PAGE-FETCH-001 分页状态
    uint m_offset=0;        // 已加载条数（下一页起点）
    uint m_total=0;         // 符合条件总人数（封顶后）
    bool m_hasMore=false;   // 还有下一页
    // REQ-PAGE-FETCH-002 搜索与封顶状态
    QString m_keyword;      // 当前搜索词（空=不过滤）
    bool m_capped=false;    // 结果总量超 LIST_TOTAL_CAP
};

#endif // ONLINEUSER_H
