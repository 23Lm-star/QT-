// 目录选择弹窗：经 LISTDIR 协议逐级浏览服务端目录，返回相对用户根的路径。
// 本对话框只发浏览请求，目录数据由外部收到响应后回灌 handleListDir。
#ifndef FOLDERSELECTDIALOG_H
#define FOLDERSELECTDIALOG_H

#include <QDialog>
#include <QString>
#include "protocol.h"

class QListWidget;
class QLabel;
class QPushButton;
class QListWidgetItem;

// 移动文件时选择目标目录的弹窗：通过 LISTDIR 协议浏览服务端目录树，
// 用户逐级进入子目录，最终"确定"返回当前浏览的相对目录路径。
class FolderSelectDialog : public QDialog {
    Q_OBJECT
public:
    explicit FolderSelectDialog(QWidget *parent = nullptr);
    QString selectedRel() const { return m_selectedRel; }

public slots:
    void handleListDir(PDU *pdu);          // 服务端 LISTDIR_RESPOND 填充列表
    void browse(const QString &rel);       // 触发浏览某相对目录（供外部在连接后调用）

signals:
    void listDirRequested(const QString &rel); // 请求浏览某相对目录

private slots:
    // 双击进入子目录
    void onItemDoubleClicked(QListWidgetItem *item);
    // 进入当前高亮目录
    void onEnterClicked();
    // 返回上一级
    void onUpClicked();
    // 确定目标目录并 accept
    void onOkClicked();
    // reject
    void onCancelClicked();

private:
    // 把浏览路径同步到顶部标签
    void updatePathLabel();

    QString m_browseRel;   // 当前浏览的相对根目录路径（空=根目录）
    QString m_selectedRel; // 确定时返回的目标相对目录
    // 子目录列表
    QListWidget *m_list;
    // 当前路径显示
    QLabel *m_pathLabel;
    QPushButton *m_upBtn;
    QPushButton *m_enterBtn;
    QPushButton *m_okBtn;
    QPushButton *m_cancelBtn;
};

#endif // FOLDERSELECTDIALOG_H
