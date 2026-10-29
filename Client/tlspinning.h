#ifndef TLSPINNING_H
#define TLSPINNING_H

#include <QSslCertificate>
#include <QSslConfiguration>
#include <QString>

// REQ-EXT-TLS-001：桌面端 TLS pinning 规则（业务规则 2/3/7 的唯一落点）。
// 抽成独立编译单元的目的：验证探针可用 g++ 编译同一份实现做断言，
// 不必启动 GUI、也不把规则复制第二份（规则改这里，客户端与探针同时生效）。
namespace tls {

// 客户端唯一信任的服务端证书 CommonName，与 Server/make_tls_certs.ps1 的 -subj 一致。
extern const char *const kVerifyName;

// 证书与私钥的固定目录约定（D4）：相对运行目录 certs/，不进取舍配置、不编进 Qt 资源。
QString certDirPath();

// 读取 certs/server.crt；缺失或无法解析返回 isNull 的证书（调用方据此回退明文并记日志）。
QSslCertificate loadPinnedCert();

// 由 pinning 证书构建客户端 TLS 配置：CA 列表只含这一张（业务规则 2/3），
// 协议下限 TLS 1.2（业务规则 7）。校验名是 QSslSocket 属性（Qt 5.14 不在配置上），
// 由调用方设为 kVerifyName。
QSslConfiguration buildPinnedConfig(const QSslCertificate &cert);

// 业务规则 5 的写出门：TLS 连接已建立 TCP 但握手未完成时禁止写出 PDU。
// mode/encrypted 的语义与 Client::connectsort/seedMsg、MyTcpSocket::recvmsg 守卫一致。
bool pduWriteBlocked(bool tlsMode, bool encrypted);

// 断线原因（D-1 修复引入）：区分「TLS 信任失败」与「其它/对端关闭」。
// Qt 的 disconnected 信号不带原因，不记录就会把证书被拒说成被互踢。
enum class DisconnectCause { Other, TlsRejected };

// D-1：是否弹「连接被服务端关闭，账号可能已在其他设备登录」这条提示。
// TLS 信任失败已有「安全连接失败」对话框，再叠一条互踢文案会把用户引向错误处置
// （去查是不是别人登录，而真实问题是证书）。重连窗保持原样：修好证书后可直接点重连。
// 调用点已保证 loggedIn 与去重，这里仍把 loggedIn 纳入真值表，使规则本身完整可断言。
bool showKickNotice(bool loggedIn, DisconnectCause cause);

// S-1：TLS 端口 = 明文主端口 + 1（D1 双端口共存）。plainPort 为 65535 时返回 false，
// 两端一律拒绝启用 TLS 并记日志——不能让服务端 listen(0) 随机挑端口还打出「启动成功」。
bool nextTlsPort(quint16 plainPort, quint16 *tlsPort);

} // namespace tls

#endif // TLSPINNING_H
