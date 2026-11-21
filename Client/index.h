#ifndef INDEX_H
#define INDEX_H

#include <QWidget>
#include "file.h"
#include "friend.h"
#include "group.h"

// 登录后的主窗口：QStackedWidget 承载好友/文件/群聊三个子页 + 注销入口。
// 布局由 index.ui 定义，子页指针（friendPage/filePage/groupPage）由 Designer 注入；
// 本类只负责页面切换与切页后的首次刷新触发，不直接处理协议回包。
namespace Ui {
class Index;
}
//索引界面（内部界面），执行各种操作
class Index : public QWidget
{
    Q_OBJECT

public:
    Index(const Index&)=delete;
    Index& operator=(const Index&)=delete;
    //单例模式，唯一实例
    static Index &getinstance();
    //获取Friend界面的ui指针
    Friend* getFriend();
    //获取File界面的ui指针
    File  * getFile();
    //获取群聊页指针（MsgHandler 据此路由群响应/事件）
    Group * getGroup();
    //获取char的界面的指针
    ~Index();
private:
    explicit Index(QWidget *parent = nullptr);
private slots:
    // 切到好友页（页 0）
    void on_friend_PB_clicked();

    // 切到文件页（页 1）并排队触发一次文件列表刷新
    void on_file_PB_clicked();

    // 切到群聊页
    void on_group_PB_clicked();

    // 注销当前账号
    void on_pushButton_clicked();

    // REQ-AUTH-CONNECTION-001：退出登录（119，账号保留，与注销严格区分）
    void on_logout_PB_clicked();

private:
    // 由 index.ui 生成的控件树（含三个子页与 stackedWidget）
    Ui::Index *ui;
};

#endif // INDEX_H
