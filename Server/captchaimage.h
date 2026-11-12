#ifndef CAPTCHAIMAGE_H
#define CAPTCHAIMAGE_H

#include <QByteArray>
#include <QString>

// REQ-AUDIT-001 R8：验证码图片渲染（服务端）。
// 文本答案只保留在服务端连接状态里，客户端仅收到 PNG 字节；图片由服务端绘制，
// 使用 QImage + QPainter（Server 已是 QApplication，字体可用）。
namespace CaptchaImage {

// 渲染 length 位验证码文本为 PNG（约 150x44），失败返回空字节。
// 同一 code 每次渲染的外观不同（干扰线与倾斜随机），但答案不变。
QByteArray renderPng(const QString &code, int width = 150, int height = 44);

// 判定一段载荷是否是 PNG（客户端/服务端共用的形态识别，按魔数判断，不新增协议常量）。
bool looksLikePng(const QByteArray &payload);

} // namespace CaptchaImage

#endif // CAPTCHAIMAGE_H
