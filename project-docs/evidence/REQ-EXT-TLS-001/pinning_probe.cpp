// REQ-EXT-TLS-001 TLS 三 / TLS 五 验证探针（无 GUI，不占 5000，不需要 MySQL）。
// 编译产品同一份 Client/tlspinning.cpp —— pinning 规则改动会同时作用于客户端与本探针。
// 连接 verify.py 起的三个 Python TLS 桩（端口经环境变量传入），断言客户端 TLS 序列的实际结果：
//   T3-a 正常证书 + 校验名一致 → encrypted 成功
//   T3-b 服务端出示未 pin 的冒名证书 → sslErrors 且不进入 encrypted
//   T3-c 证书可信但校验名不符 → HostNameMismatch 拒绝
//   T3-d 证书过期 → CertificateExpired 拒绝（pin 的就是这张过期证书，拒因只剩过期）
//   T3-e 规则形态：CA 只含那一张 + 协议下限 TLS1.2 + 校验名写入 socket
//   T3-f 业务规则 5：TLS 握手未完成时写出被拦，握手后放开，明文连接不受影响
//   T5-a TLS 层硬断连后 socket 释放、状态收敛（不悬挂）
//   T5-b 断连后服务端仍可完成新握手
// 「服务端是否真的没完成握手」由 verify.py 用 Python 桩的握手计数交叉断言；
// 客户端侧不以本地计数为判据——客户端在证书校验阶段拒绝时，服务端那侧可能已记为完成。
#include <QCoreApplication>
#include <QTcpSocket>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslError>
#include <QStringList>
#include <QFile>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include <cstdio>
#include "tlspinning.h"

static int g_pass = 0, g_fail = 0;

static void report(const char *name, bool ok, const QString &detail)
{
    if (ok) ++g_pass; else ++g_fail;
    printf("%s %s :: %s\n", ok ? "[PASS]" : "[FAIL]", name, detail.toUtf8().constData());
    fflush(stdout);
}

static QByteArray readAll(const char *p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    const QByteArray d = f.readAll();
    f.close();
    return d;
}

static quint16 portEnv(const char *name)
{
    const char *v = getenv(name);
    return v ? quint16(QByteArray(v).toUInt()) : 0;
}

// 与 Client::startConnection 同一序列：setSslConfiguration(tls::buildPinnedConfig(...))
// + setPeerVerifyName(tls::kVerifyName)。客户端带 UI 依赖无法在此编译，故把该序列集中一处，
// 客户端改动必须与本探针同步（否则 T3-e 的形态断言会失配）。
// TLS_PROBE_MODE=spike 切成追加式 CA，仅供人工对照定位，不参与默认判定。
static void applyClientTls(QSslSocket *sock, const QSslCertificate &cert, const char *verifyName)
{
    const QString mode = QString::fromLatin1(getenv("TLS_PROBE_MODE") ? getenv("TLS_PROBE_MODE") : "cfg");
    if (mode == "spike") {
        sock->addCaCertificates(QList<QSslCertificate>() << cert);
    } else {
        sock->setSslConfiguration(tls::buildPinnedConfig(cert));
    }
    sock->setPeerVerifyName(verifyName);
}

// 一次客户端 TLS 尝试：settleMs 到点后 abort + 释放 socket，再把结果交回回调。
// encrypted/errors 在释放前采集，回调读到的是既成事实。
struct Attempt {
    QSslSocket *sock;
    QList<QSslError> errors;
    QString sockErrText;
    bool encrypted = false;
    qint64 ms = -1;
    QElapsedTimer watch;
    std::function<void(Attempt*)> onSettled;

    Attempt(const QSslCertificate &pin, const char *verifyName, quint16 port, int settleMs,
            std::function<void(Attempt*)> fn)
        : sock(new QSslSocket), onSettled(std::move(fn))
    {
        applyClientTls(sock, pin, verifyName);
        watch.start();
        QObject::connect(sock, &QSslSocket::encrypted, [this] {
            if (!encrypted) { encrypted = true; ms = watch.elapsed(); }
        });
        QObject::connect(sock, QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
                         [this](const QList<QSslError> &e) { errors = e; });
        QObject::connect(sock, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
                         [this](QAbstractSocket::SocketError) {
            sockErrText = sock ? sock->errorString() : QString();
        });
        sock->connectToHostEncrypted("127.0.0.1", port);
        QTimer::singleShot(settleMs, [this] { settle(); });
    }
    void settle()
    {
        std::function<void(Attempt*)> fn = onSettled;
        onSettled = nullptr;
        if (sock) { sock->abort(); delete sock; sock = nullptr; }
        if (fn) fn(this);
        delete this;
    }
    bool hasErrorKind(QSslError::SslError code) const
    {
        for (const QSslError &e : errors) if (e.error() == code) return true;
        return false;
    }
    QString errorKinds() const
    {
        QStringList out;
        for (const QSslError &e : errors) out << QString::number((int)e.error());
        return out.join(',');
    }
    QString firstError() const { return errors.isEmpty() ? QString() : errors.value(0).errorString(); }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const char *SRV_CRT = getenv("TLS_SRV_CRT"), *EXP_CRT = getenv("TLS_EXP_CRT");
    const quint16 okPort = portEnv("TLS_OK_PORT"), rogPort = portEnv("TLS_ROG_PORT"),
                    expPort = portEnv("TLS_EXP_PORT");
    if (!SRV_CRT || !EXP_CRT || !okPort || !rogPort || !expPort) {
        printf("[FAIL] setup :: 缺少 TLS_SRV_CRT/TLS_EXP_CRT 或 TLS_{OK,ROG,EXP}_PORT（由 verify.py 注入）\n");
        return 2;
    }
    const QByteArray pem = readAll(SRV_CRT);
    printf("[CHK] crt bytes=%d supportsSsl=%d runtime=%s\n", (int)pem.size(),
           QSslSocket::supportsSsl() ? 1 : 0,
           QSslSocket::sslLibraryVersionString().toUtf8().constData());
    fflush(stdout);
    if (pem.isEmpty()) {
        printf("[FAIL] setup :: 证书读取为空——路径必须纯 ASCII，"
               "中文用户名经 getenv 是 GBK 字节，Qt 按 UTF-8 解释会打不开\n");
        return 4;
    }
    const QSslCertificate pinned = QSslCertificate(pem);
    const QSslCertificate pinnedExp = QSslCertificate(readAll(EXP_CRT));
    if (pinned.isNull() || pinnedExp.isNull()) {
        printf("[FAIL] setup :: 证书解析为空\n");
        return 5;
    }

    // ---- T3-e 规则形态（直接调产品实现，规则漂移会被这里抓到）----
    {
        const QSslConfiguration cfg = tls::buildPinnedConfig(pinned);
        const bool singleCa = (cfg.caCertificates().size() == 1 && cfg.caCertificates().at(0) == pinned);
        const bool proto = (cfg.protocol() == QSsl::TlsV1_2);
        const bool nameConst = (QString::fromLatin1(tls::kVerifyName) == QStringLiteral("tlsserver"));
        QSslSocket probeSock;
        applyClientTls(&probeSock, pinned, tls::kVerifyName);
        const bool sockName = (probeSock.peerVerifyName() == QStringLiteral("tlsserver"));
        const int anchors = probeSock.sslConfiguration().caCertificates().size();
        report("T3-e-rule-shape", singleCa && proto && nameConst && sockName,
               QString("caCount=%1 protocol=%2 nameConst=%3 sockPeerName=%4")
                   .arg(cfg.caCertificates().size()).arg((int)cfg.protocol()).arg(nameConst).arg(sockName));
        report("T3-e-single-anchor", anchors == 1,
               QString("信任锚数量=%1（必须只剩 pin 的那一张，不叠加系统 CA）").arg(anchors));
    }

    // ---- T3-f 业务规则 5 写出闸门 ----
    report("T3-f-write-gate",
           tls::pduWriteBlocked(true, false) && !tls::pduWriteBlocked(true, true)
           && !tls::pduWriteBlocked(false, false),
           QString("tls握手前=%1 tls握手后=%2 明文=%3").arg(
               tls::pduWriteBlocked(true, false)).arg(tls::pduWriteBlocked(true, true))
               .arg(tls::pduWriteBlocked(false, false)));

    // ---- T3-a → T3-b → T3-c → T3-d → T5-a → T5-b 串行接力 ----
    new Attempt(pinned, tls::kVerifyName, okPort, 3000, [&](Attempt *a) {
        report("T3-a-happy", a->encrypted,
               QString("encrypted=%1 ms=%2 sslErr=%3 sockErr=%4").arg(a->encrypted)
                   .arg(a->ms).arg(a->firstError()).arg(a->sockErrText));
        new Attempt(pinned, tls::kVerifyName, rogPort, 3000, [&](Attempt *b) {
            report("T3-b-rogue-rejected", !b->encrypted && !b->errors.isEmpty(),
                   QString("encrypted=%1 errors=%2 kinds=%3 first=%4").arg(b->encrypted)
                       .arg(b->errors.size()).arg(b->errorKinds()).arg(b->firstError()));
            new Attempt(pinned, "otherserver", okPort, 3000, [&](Attempt *c) {
                report("T3-c-name-mismatch",
                       !c->encrypted && c->hasErrorKind(QSslError::HostNameMismatch),
                       QString("encrypted=%1 nameMismatch=%2 kinds=%3").arg(c->encrypted)
                           .arg(c->hasErrorKind(QSslError::HostNameMismatch)).arg(c->errorKinds()));
                new Attempt(pinnedExp, tls::kVerifyName, expPort, 3000, [&](Attempt *d) {
                    report("T3-d-expired-rejected",
                           !d->encrypted && d->hasErrorKind(QSslError::CertificateExpired),
                           QString("encrypted=%1 expired=%2 kinds=%3").arg(d->encrypted)
                               .arg(d->hasErrorKind(QSslError::CertificateExpired)).arg(d->errorKinds()));
                    // T5-a：握手完成后 TLS 层硬断（settle 内 abort），socket 必须释放、不悬挂
                    new Attempt(pinned, tls::kVerifyName, okPort, 1500, [&](Attempt *e) {
                        report("T5-a-abort-converges", e->encrypted && e->sock == nullptr,
                               QString("encrypted=%1 released=%2 ms=%3").arg(e->encrypted)
                                   .arg(e->sock == nullptr).arg(e->ms));
                        new Attempt(pinned, tls::kVerifyName, okPort, 3000, [&](Attempt *f) {
                            report("T5-b-server-alive-after-abort", f->encrypted,
                                   QString("encrypted=%1 ms=%2 err=%3").arg(f->encrypted)
                                       .arg(f->ms).arg(f->sockErrText));
                            QCoreApplication::exit(g_fail == 0 ? 0 : 1);
                        });
                    });
                });
            });
        });
    });

    QTimer::singleShot(30000, [] {
        report("watchdog", false, "探针未在 30s 内结束");
        QCoreApplication::exit(3);
    });
    return app.exec();
}
