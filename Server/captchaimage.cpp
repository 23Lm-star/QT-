#include "captchaimage.h"

#include <QBuffer>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QRandomGenerator>

// captchaimage.cpp — REQ-AUDIT-001 R8 图片验证码渲染。
// 不引入额外依赖：QImage + QPainter + QBuffer(PNG)，字体来自进程内 QApplication。
namespace {

int randInt(int low, int high)
{
    if (high <= low) return low;
    return low + int(QRandomGenerator::global()->bounded(high - low + 1));
}

QColor randColor(int minLum, int maxLum)
{
    int r = randInt(minLum, maxLum);
    int g = randInt(minLum, maxLum);
    int b = randInt(minLum, maxLum);
    return QColor(r, g, b);
}

} // namespace

QByteArray CaptchaImage::renderPng(const QString &code, int width, int height)
{
    if (code.isEmpty() || width <= 0 || height <= 0) {
        return QByteArray();
    }
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(randInt(235, 250), randInt(235, 250), randInt(235, 250)));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // 干扰线：3~5 条，浅色，避免覆盖字符
    const int lines = randInt(3, 5);
    for (int i = 0; i < lines; ++i) {
        QPen pen(randColor(120, 200));
        pen.setWidth(randInt(1, 2));
        painter.setPen(pen);
        painter.drawLine(randInt(0, width / 3), randInt(0, height),
                         randInt(width * 2 / 3, width), randInt(0, height));
    }
    // 干扰点：40 个
    for (int i = 0; i < 40; ++i) {
        painter.setPen(randColor(90, 200));
        painter.drawPoint(randInt(0, width - 1), randInt(0, height - 1));
    }

    QFont font;
    font.setFamily(QStringLiteral("Microsoft YaHei"));
    font.setBold(true);
    font.setPixelSize(int(height * 0.62));
    painter.setFont(font);

    const int cell = width / int(code.size());
    for (int i = 0; i < code.size(); ++i) {
        painter.save();
        const QChar ch = code.at(i);
        painter.setPen(randColor(20, 120));
        const QPointF center(i * cell + cell / 2.0, height / 2.0 + randInt(-3, 3));
        painter.translate(center);
        painter.rotate(randInt(-14, 14));
        painter.drawText(QRectF(-cell / 2.0, -height / 2.0, cell, height),
                         Qt::AlignCenter, QString(ch));
        painter.restore();
    }
    painter.end();

    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return QByteArray();
    }
    if (!image.save(&buffer, "PNG")) {
        return QByteArray();
    }
    buffer.close();
    return png;
}

bool CaptchaImage::looksLikePng(const QByteArray &payload)
{
    static const char kSig[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
    if (payload.size() < 8) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (payload.at(i) != kSig[i]) {
            return false;
        }
    }
    return true;
}
