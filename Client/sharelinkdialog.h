// 分享链接展示弹窗：只读显示链接并提供一键复制，链接为空时禁用复制。
#ifndef SHARELINKDIALOG_H
#define SHARELINKDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QApplication>
#include <QClipboard>
#include <QToolTip>

// 纯代码构建的对话框；filePath 仅用于顶部说明文字
class ShareLinkDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShareLinkDialog(const QString &filePath, QWidget *parent = nullptr);
    // 更新链接；空串表示当前无链接（清空并禁用复制）
    void setLink(const QString &link);

private slots:
    // 复制当前链接到系统剪贴板并给出气泡提示
    void onCopyClicked();

private:
    // 只读链接框
    QLineEdit *m_linkEdit;
    // 复制按钮（无链接时禁用）
    QPushButton *m_copyBtn;
};

#endif // SHARELINKDIALOG_H
