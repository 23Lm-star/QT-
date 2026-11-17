#ifndef FRIEND_H
#define FRIEND_H

#include "onlineuser.h"

#include <QWidget>
#include <QShowEvent>
#include <QTimer>
#include <QStringList>
#include "chat.h"

// 好友页：好友列表的查询/删除/发起私聊，以及“所有在线用户”子窗口的入口。
// 列表数据由 MsgHandler 回包后经 flushFriend_LW 缓存，真正写控件只在 rebuildFriendList；
// 页面不可见时仅置脏标志，待 showEvent 再合并刷新（避免后台反复重绘）。
namespace Ui {
class Friend;
}
//好友类
// 好友管理 + 私聊入口
class Friend : public QWidget
{
    Q_OBJECT

public:
    //在线用户的子框架的指针
    OnlineUser* m_onlineUser;
    //聊天界面的指针
    Chat* m_chat;
    //刷新好友接口函数->引导刷新好友
    void flushfriend();
    // REQ-CHAT-ACK-001 R10：拉取待处理好友申请（登录与刷新好友时调用）
    void requestPendingFriendRequests();
    // 返回好友列表控件（供外部/测试直接访问，正常刷新走 rebuildFriendList）
    QListWidget* getfile_LW();
    //刷新好友操作函数——仅接管数据，不直接写 QListWidget
    // REQ-PAGE-FETCH-002：q 为本页纯用户名（「序号.用户名」编号跨页连续由本函数追加）；
    // total/hasMore/capped 为分页与总量封顶元数据
    void flushFriend_LW(const QStringList& q, uint total, bool hasMore, bool capped);
    explicit Friend(QWidget *parent = nullptr);
    ~Friend();

protected:
    // 页面重新可见时补做一次合并刷新（不可见期间只缓存数据）
    void showEvent(QShowEvent *event) override;

private slots:
    //查询好友槽函数
    void on_findUser_PB_clicked();
    //获取所有用户（非好友）
    void on_onlineUser_PB_clicked();
    //刷新好友槽函数
    void on_flush_PB_clicked();
    //删除好友槽函数
    void on_del_PB_clicked();
    //聊天槽函数
    void on_chat_PB_clicked();
    // REQ-PAGE-FETCH-002：加载更多好友（按已加载条数请求下一页）
    void on_loadMore_PB_clicked();

private:
    Ui::Friend *ui;
    // REQ-PAGE-FETCH-002：按 limit=50 请求指定偏移页
    void requestFriendPage(uint offset);
    // 缓存的好友列表数据（「序号.用户名」，编号跨页连续）
    QStringList m_friendList;
    // REQ-PAGE-FETCH-002 分页状态（已加载条数=m_friendList.size()）
    uint m_friendTotal=0;       // 好友总数（封顶后）
    bool m_friendHasMore=false; // 还有下一页
    bool m_friendCapped=false;  // 好友总数超 LIST_TOTAL_CAP
    // 存在待处理的 UI 刷新数据
    bool m_pendingUiRefresh = false;
    // 已有一个 UI 刷新排队中
    bool m_uiRefreshQueued = false;
    // 请求一次合并排队 UI 刷新
    void scheduleUiRefresh();
    // 唯一可写 QListWidget 的私有方法
    void rebuildFriendList();
};

#endif // FRIEND_H
