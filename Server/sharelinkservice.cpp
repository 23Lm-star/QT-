#include "sharelinkservice.h"
#include <QByteArray>

// sharelinkservice.cpp — 分享链接令牌的简单实现。
// 令牌 = 16 字节系统随机数（URL-safe Base64），落库前经 SHA-256 转为 hex。

QString ShareLinkService::generateToken() {
    // 128 bit 熵，足够避免在线枚举。
    QByteArray randomData(16, 0);
    for (int i = 0; i < 16; ++i) {
        randomData[i] = static_cast<char>(QRandomGenerator::system()->generate() % 256);
    }
    // URL-safe Base64: + -> -, / -> _, remove =
    return randomData.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

// 数据库只保存摘要；分享访问时对来客令牌重算并比对。
QString ShareLinkService::hashToken(const QString &token) {
    return QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex();
}

// 默认有效期 24 小时（86400 秒）。
QDateTime ShareLinkService::defaultExpireTime() {
    return QDateTime::currentDateTime().addSecs(24 * 3600);
}
