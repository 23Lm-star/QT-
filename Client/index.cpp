#include "index.h"
#include "ui_index.h"
#include "client.h"
#include <QTimer>

// 主窗口实现：单例供 MsgHandler/Client 全局取子页；
// 页面切换只改 stackedWidget 索引，好友页靠 showEvent、文件页靠事件队列触发刷新。
Index::Index(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Index)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
    // REQ-UX-001 A5：2 倍放大并收敛到屏幕可用区
    setFixedSize(Client::scaledWindowSize(1000,650));
}
Index &Index::getinstance()
{
    // C++11 局部静态：首次调用时构造，进程生命周期内唯一
    static Index instance;
    return instance;
}

Friend *Index::getFriend()
{
    // 由 index.ui 提升/嵌入的好友页实例
    return ui->friendPage;
}

File *Index::getFile()
{
    return ui->filePage;
}

Group *Index::getGroup()
{
    return ui->groupPage;
}

Index::~Index()
{
    delete ui;
}

void Index::on_friend_PB_clicked()
{
    // 页 0 = 好友页；切页后其 showEvent 会按需刷新
    ui->stackedWidget->setCurrentIndex(0);
}

void Index::on_file_PB_clicked()
{
    // 页 1 = 文件页；文件列表不随页面常驻刷新，切页时主动拉一次
    ui->stackedWidget->setCurrentIndex(1);
    // 切换到文件页后，通过事件队列下一轮触发文件列表刷新
    // 确保页面已成为当前可见页后再发送网络请求
    QTimer::singleShot(0, [this](){
        ui->filePage->flushfile();
    });
}

void Index::on_group_PB_clicked()
{
    // 用控件指针而非索引，避免 .ui 页序调整后错位
    ui->stackedWidget->setCurrentWidget(ui->groupPage);
}
void Index::on_pushButton_clicked()
{
    // 注销：Client 处理回包后负责断开连接并回到登录界面
    Client::getinstance().sendLogoffRequest();
}
