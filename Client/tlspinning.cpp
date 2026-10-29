#include "tlspinning.h"

#include <QCoreApplication>
#include <QFile>
#include <QList>

namespace tls {

const char *const kVerifyName = "tlsserver";

QString certDirPath()
{
    return QCoreApplication::applicationDirPath() + "/certs";
}

QSslCertificate loadPinnedCert()
{
    const QString crtPath = certDirPath() + "/server.crt";
    QFile cf(crtPath);
    if (!cf.open(QIODevice::ReadOnly)) {
        return QSslCertificate();
    }
    const QByteArray pem = cf.readAll();
    cf.close();
    return QSslCertificate(pem);
}

QSslConfiguration buildPinnedConfig(const QSslCertificate &cert)
{
    QSslConfiguration cfg;
    cfg.setCaCertificates(QList<QSslCertificate>() << cert);   // pinning：只信这一张
    cfg.setProtocol(QSsl::TlsV1_2);                            // 业务规则 7：最低 TLS 1.2
    return cfg;
}

bool pduWriteBlocked(bool tlsMode, bool encrypted)
{
    return tlsMode && !encrypted;   // 业务规则 5
}

bool showKickNotice(bool loggedIn, DisconnectCause cause)
{
    return loggedIn && cause != DisconnectCause::TlsRejected;   // D-1
}

bool nextTlsPort(quint16 plainPort, quint16 *tlsPort)
{
    if (plainPort == 0 || plainPort == 65535) return false;     // S-1：溢出与 0 都不接受
    if (tlsPort) *tlsPort = static_cast<quint16>(plainPort + 1);
    return true;
}

} // namespace tls
