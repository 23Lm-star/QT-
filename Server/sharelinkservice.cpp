#include "sharelinkservice.h"
#include <QByteArray>

// sharelinkservice.cpp — 分享链接令牌与路径校验的简单实现。
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

// 先确认文件存在，再比较 canonical 路径，确保 ".." 不能逃出 ownerRoot。
bool ShareLinkService::validatePath(const QString &ownerRoot, const QString &relativePath) {
    QDir rootDir(ownerRoot);
    QString absolutePath = rootDir.absoluteFilePath(relativePath);
    QFileInfo fileInfo(absolutePath);
    
    // 检查文件是否存在
    if (!fileInfo.exists()) return false;
    
    // 检查是否在根目录下（防止路径穿越）
    return absolutePath.startsWith(rootDir.canonicalPath());
}

// 默认有效期 24 小时（86400 秒）。
QDateTime ShareLinkService::defaultExpireTime() {
    return QDateTime::currentDateTime().addSecs(24 * 3600);
}
