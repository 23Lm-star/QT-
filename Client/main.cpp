#include "client.h"
#include "index.h"

#include <QApplication>
#include <QFile>
#include <QString>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 加载全局 QSS 炫酷主题
    QFile qssFile(":/style.qss");
    if (qssFile.open(QFile::ReadOnly)) {
        QString qssContent = QString::fromUtf8(qssFile.readAll());
        a.setStyleSheet(qssContent);
        qssFile.close();
    } else {
        qWarning() << "QSS 加载失败: 未找到资源 :/style.qss";
    }

    //通过架构函数来获取唯一示例
    Client::getinstance().show();
    return a.exec();
}
