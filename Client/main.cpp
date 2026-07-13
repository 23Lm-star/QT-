#include "client.h"
#include "index.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    //通过架构函数来获取唯一示例
    Client::getinstance().show();
    return a.exec();
}
