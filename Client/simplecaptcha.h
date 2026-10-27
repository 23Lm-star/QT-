// 简单图形验证码控件：显示随机大写字母/数字串，校验时忽略大小写。
// captcha_simple.h
#ifndef CAPTCHA_SIMPLE_H
#define CAPTCHA_SIMPLE_H

#include <QWidget>
#include <QLabel>
#include <QString>

// 仅文本展示，不含扭曲/干扰线；用于登录/注册的基本人机校验
class SimpleCaptcha : public QWidget
{
    Q_OBJECT
public:
    explicit SimpleCaptcha(QWidget *parent = nullptr);

    // 取当前验证码文本
    QString getCaptchaCode() const { return m_code; }
    // 设置长度（限制在 4..10）；长度变化时立即重新生成
    void setCodeLength(int length);
    // 忽略大小写比较
    bool verify(const QString &input) const;
    void refreshCaptcha();   // 刷新验证码

signals:
    // 验证码重新生成后发出（外部可据此清空输入框等）
    void captchaRefreshed();

private:
    QLabel *m_label;         // 用于显示验证码文本
    QString m_code;          // 当前验证码
    int m_codeLength;        // 验证码长度

    // 生成随机验证码（使用全局 QRandomGenerator）
    QString generateRandomCode(); // 生成随机验证码
};

#endif // CAPTCHA_SIMPLE_H
