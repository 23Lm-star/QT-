#ifndef FRIEND_H
#define FRIEND_H

#include "onlineuser.h"

#include <QWidget>
#include "chat.h"
namespace Ui {
class Friend;
}
//好友类
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
    QListWidget* getfile_LW();
    //刷新好友操作函数
    void flushFriend_LW(const QStringList& q);
    explicit Friend(QWidget *parent = nullptr);
    ~Friend();
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

private:
    Ui::Friend *ui;
};

#endif // FRIEND_H
