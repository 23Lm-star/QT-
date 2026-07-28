#ifndef FILE_H
#define FILE_H
#include "protocol.h"
#include <QWidget>
#include <QShowEvent>
#include <qlistwidget.h>
#include "sharefile.h"
namespace Ui {
class File;
}
//文件类:
class File : public QWidget
{
    Q_OBJECT

public:
    QString m_strUserPath;//用户路径
    QString m_strCurPath;//当前路径
    QString m_strUploadPath;//上传文件路径
    QString m_strShareFileName;//分享文件名
    QList<FileInfo*>m_fileInfoList;//当前路径文件信息
    ShareFile* m_shareFile;
    void flushfile();//刷新当前文件->引导刷新
    void uploadfile(qint64 resumeOffset=0);//上传文件数据->上传m_strUploadPath文件
    void updateCurrfile(QList<FileInfo*>qFileinfolist);//利用文件信息列表刷新文件信息
    void flushCurrLE();
    explicit File(QWidget *parent = nullptr);
    ~File();
protected:
    void showEvent(QShowEvent *event) override;//页面变为可见时重建待刷新列表
private slots:
    void errnoMsg(const QString& Msg);
    //创建目录槽函数
    void on_credit_PB_clicked();
    //刷新文件槽函数
    void on_flush_PB_clicked();
    //删除文件槽函数
    void on_del_PB_clicked();
    //进入当前文件/目录槽函数
    void on_file_LW_itemDoubleClicked(QListWidgetItem *item);
    //返回上级目录槽函数
    void on_back_PB_clicked();
    //重命名文件/目录槽函数
    void on_rename_PB_clicked();

    void on_inputPath_PB_clicked();

    void on_mkdir_PB_clicked();

    void on_download_PB_clicked();

    void on_share_PB_clicked();

private:
    Ui::File *ui;
    bool m_pendingUiRefresh = false;//标记：存在待刷新的文件列表数据
    bool m_uiRefreshQueued = false;//防止重复排入 UI 重建回调
    void rebuildFileList();//集中执行 QListWidget clear() + addItem()
    void scheduleUiRefresh();//合并排队：最多排入一个 UI 刷新回调
};

#endif // FILE_H
