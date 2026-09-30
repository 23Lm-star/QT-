#include "index.h"
#include "ui_index.h"
#include "client.h"
#include <QTimer>

Index::Index(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Index)
{
    ui->setupUi(this);
    setFixedSize(1000,650);
}
Index &Index::getinstance()
{
    static Index instance;
    return instance;
}

Friend *Index::getFriend()
{
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
    ui->stackedWidget->setCurrentIndex(0);
}

void Index::on_file_PB_clicked()
{
    ui->stackedWidget->setCurrentIndex(1);
    // 切换到文件页后，通过事件队列下一轮触发文件列表刷新
    // 确保页面已成为当前可见页后再发送网络请求
    QTimer::singleShot(0, [this](){
        ui->filePage->flushfile();
    });
}

void Index::on_group_PB_clicked()
{
    ui->stackedWidget->setCurrentWidget(ui->groupPage);
}
void Index::on_pushButton_clicked()
{
    Client::getinstance().sendLogoffRequest();
}
