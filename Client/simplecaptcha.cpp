// 验证码控件实现：字符集剔除 0/O/1/I 等易混淆字符，生成即刷新显示。
// captcha_simple.cpp
#include "simplecaptcha.h"
#include <QRandomGenerator>
#include <QTime>
#include <qboxlayout.h>

// 构造：默认 4 位，初始化标签样式并生成首个验证码
SimpleCaptcha::SimpleCaptcha(QWidget *parent)
    : QWidget(parent), m_codeLength(4)
{
    // 创建显示验证码的标签
    m_label = new QLabel(this);
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setStyleSheet(
        "QLabel {"
        "   font-family: 'Consolas', monospace;"
        "   font-size: 28px;"
        "   font-weight: bold;"
        "}"
    );
    m_label->setFixedSize(240, 80);   // REQ-UX-001 A5：面积 4 倍

    // 初始化随机种子（可选）
    static bool seeded = false;
    if (!seeded) {
        QRandomGenerator::global()->seed(QTime::currentTime().msec());
        seeded = true;
    }

    refreshCaptcha();

    // 布局：让标签填满当前控件
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->addWidget(m_label);
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);
}

// 重新生成并广播 captchaRefreshed
void SimpleCaptcha::refreshCaptcha()
{
    m_code = generateRandomCode();
    m_label->setText(m_code);
    emit captchaRefreshed();
}

// 长度夹在 [4,10]；仅在变化时刷新，避免无谓重生成
void SimpleCaptcha::setCodeLength(int length)
{
    int clamped = qBound(4, length, 10);
    if (clamped != m_codeLength) {
        m_codeLength = clamped;
        refreshCaptcha();
    }
}

// 校验：大小写不敏感（字符集本身为大写）
bool SimpleCaptcha::verify(const QString &input) const
{
    return m_code.compare(input, Qt::CaseInsensitive) == 0;
}

// 逐位从字符集均匀抽取；每字符独立生成
QString SimpleCaptcha::generateRandomCode()
{
    // 字符集：大写字母 + 数字（排除易混淆字符）
    const QString possibleChars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    QString code;
    for (int i = 0; i < m_codeLength; ++i) {
        int index = QRandomGenerator::global()->bounded(possibleChars.length());
        code.append(possibleChars.at(index));
    }
    return code;
}
