#ifndef SHARELINKSERVICE_H
#define SHARELINKSERVICE_H

#include <QString>
#include <QDateTime>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

// 分享链接工具集（无状态静态方法）：生成/散列令牌、校验分享路径、计算过期时间。
// 令牌只暴露给客户端一次，服务端应按 hashToken 的结果落库，避免明文可用令牌泄露。
class ShareLinkService {
public:
    static QString generateToken();        // 16 随机字节 -> URL-safe Base64
    static QString hashToken(const QString &token); // SHA-256 -> hex
    // 相对路径必须存在且 canonical 后仍位于 ownerRoot 内（防目录穿越）。
    static bool validatePath(const QString &ownerRoot, const QString &relativePath); // canonical 校验
    static QDateTime defaultExpireTime();  // 当前时间 + 24h
};

#endif // SHARELINKSERVICE_H
