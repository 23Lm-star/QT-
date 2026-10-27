// 分享链接弹窗实现：链接由外部 setLink 注入，弹窗本身不发协议请求。
#include "sharelinkdialog.h"

// 构建「文件名 + 只读链接框 + 复制按钮」的简单布局
ShareLinkDialog::ShareLinkDialog(const QString &filePath, QWidget *parent) : QDialog(parent) {
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    setWindowTitle("分享文件");
    setMinimumWidth(400);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QString("文件：%1").arg(filePath)));
    
    m_linkEdit = new QLineEdit(this);
    m_linkEdit->setReadOnly(true);
    m_linkEdit->setPlaceholderText("暂无分享链接，请先创建");
    layout->addWidget(m_linkEdit);

    m_copyBtn = new QPushButton("复制链接", this);
    m_copyBtn->setEnabled(false);
    layout->addWidget(m_copyBtn);

    connect(m_copyBtn, &QPushButton::clicked, this, &ShareLinkDialog::onCopyClicked);
}

// 同步 UI 状态：链接为空与可复制互斥
void ShareLinkDialog::setLink(const QString &link) {
    if (link.isEmpty()) {
        m_linkEdit->clear();
        m_copyBtn->setEnabled(false);
    } else {
        m_linkEdit->setText(link);
        m_copyBtn->setEnabled(true);
    }
}

// 复制到剪贴板；气泡定位在按钮左上角
void ShareLinkDialog::onCopyClicked() {
    QApplication::clipboard()->setText(m_linkEdit->text());
    QToolTip::showText(m_copyBtn->mapToGlobal(QPoint(0, 0)), "已复制到剪贴板");
}
