// 好友分享弹窗：选择接收人后发 SHAREFILE 请求，把当前文件分享给选中好友。
#ifndef SHAREFILE_H
#define SHAREFILE_H

#include <QWidget>

namespace Ui {
class ShareFile;
}

class ShareFile : public QWidget
{
    Q_OBJECT

public:
    // 待分享文件名（由 File 页写入）
    QString m_strsharedName;
    explicit ShareFile(QWidget *parent = nullptr);
    ~ShareFile();
    // 从好友列表重建接收人列表（去掉状态后缀）
    void flusfriend();
private slots:
    // 全选接收人
    void on_allchoose_PB_clicked();

    // 清空选择
    void on_nochoose_PB_clicked();

    // 发送分享请求
    void on_output_PB_clicked();

private:
    Ui::ShareFile *ui;
};

#endif // SHAREFILE_H
