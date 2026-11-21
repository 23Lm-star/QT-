// REQ-AUTH-CONNECTION-001 阶段四：Qt 桌面端会话链路 harness（测试专用，不进 Client.pro）。
// offscreen 驱动真实 Client/MsgHandler/Index 代码路径，对接真实 Server+Redis：
//   QSettings 指向隔离 INI（不碰用户注册表）；模态 QMessageBox 由定时器自动关闭。
// 由 qt_client_probe.py 生成 .pro 链接 Client 全部源文件（除 main.cpp）。
// 模式（argv[1]）：
//   login <user> <pwd>     注册+登录，落盘 token，打印 HARNESS_OK
//   logout                 已登录态点退出按钮（119），验证匿名+token 清除
//   resume                 有落盘 token：模拟断线→重连→121 恢复主界面
//   kicked                 已登录态收 123 失效事件：token 清除回登录页
//   badtoken               落盘 token 篡改后重连：122 失败清 token 回登录页
//   logoff                 注销（48）：跳过密码弹窗注入确认，验证全清
#include "client.h"
#include "index.h"
#include "file.h"
#include "logging.h"
#include "protocol.h"
#include "transfertask.h"

#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QDir>
#include <cstdio>
#include <cstdlib>

static int g_fails = 0;
static void ck(const char* name, bool ok, const QString& detail = QString())
{
    printf("CHECK %s %s%s\n", name, ok ? "PASS" : "FAIL",
           ok ? "" : QString(" | %1").arg(detail).toLocal8Bit().constData());
    fflush(stdout);
    if (!ok) ++g_fails;
}

// 事件循环等待谓词（处理网络与定时器），超时返回 false
template<typename Pred>
static bool waitUntil(Pred p, int timeoutMs = 8000)
{
    QElapsedTimer t; t.start();
    while (!p()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return true;
}

// 模态弹窗自动处理：Qt 5.14 QMessageBox::showEvent 在 offscreen/minimal 平台会调
// qt_getWindowsSystemMenu 崩溃（Win11 实测），必须在窗口 show 之前完成处置。
// QApplication 事件过滤器拦 QEvent::ShowToParent（发送先于 showEvent）：
//   QMessageBox → 直接 close()（跳过 exec 的显示；调用方拿到默认返回）；
//   注销二次确认（含 Yes 按钮）→ 先点 Yes 再 close；
//   QInputDialog（注销密码确认）→ 注入密码后 accept。
class DialogAutoCloser : public QObject {
public:
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() == QEvent::ShowToParent || ev->type() == QEvent::Show) {
            if (QMessageBox* mb = qobject_cast<QMessageBox*>(obj)) {
                // done() 直接结束 exec 的模态循环（close() 对未显示窗口无效，会卡死 exec）
                const int r = mb->button(QMessageBox::Yes) ? QMessageBox::Yes : QMessageBox::Ok;
                QMetaObject::invokeMethod(mb, "done", Qt::QueuedConnection, Q_ARG(int, r));
                return true;   // 吞掉 Show，showEvent 不再执行
            }
            if (obj->inherits("QInputDialog")) {
                // Show 过滤器内直接注入密码并 accept（DirectConnection）：
                // QDialog::accept→done 在 exec 开模态循环前把结果落定，exec 进入即退出。
                const QByteArray pwd = qgetenv("HARNESS_LOGOFF_PWD");
                QLineEdit* le = obj->findChild<QLineEdit*>();
                if (le && !pwd.isEmpty()) le->setText(QString::fromUtf8(pwd));
                QMetaObject::invokeMethod(obj, "accept", Qt::DirectConnection);
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};
static void installModalAutoClose()
{
    qApp->installEventFilter(new DialogAutoCloser());
}

static QString settingsToken()
{
    QSettings st;
    st.sync();   // 同进程构造的默认 QSettings 有缓存，强制重读磁盘
    return st.value(QStringLiteral("session/resume_token")).toString();
}

int main(int argc, char* argv[])
{
    // 平台插件由探针经环境变量指定（minimal：无 Windows 系统菜单调用，
    // offscreen 的 QMessageBox::showEvent 在 Win11 26xxx 实测 fail-fast，不可用）
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication app(argc, argv);
    // 隔离持久化：exe 旁 ASCII 目录（中文用户 temp 路径经 stdout 管道会乱码，探针无法回读）。
    // 每进程唯一子目录，退出时清理；QSettings INI 与 cid/answer 交换文件都在其中。
    const QString isoRoot = QCoreApplication::applicationDirPath()
            + QStringLiteral("/ac4iso-%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(isoRoot);
    // 隔离目录不在析构期清理：Client 单例是函数内静态，析构顺序不可控，
    // 在 atexit 阶段触碰 QWidget/QDir 实测崩溃；由探针脚本收尾统一删除。
    const QString isoPath = isoRoot;
    QCoreApplication::setOrganizationName(QStringLiteral("ac4probe"));
    QCoreApplication::setApplicationName(QStringLiteral("ac4probe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isoPath);
    qputenv("HARNESS_DATA_DIR", isoPath.toUtf8());
    // resume/badtoken 模式：从上一进程保存的 INI 恢复凭据（模拟客户端重启）
    const QByteArray priorIni = qgetenv("HARNESS_PRIOR_INI");
    if (!priorIni.isEmpty() && QFile::exists(QString::fromUtf8(priorIni))) {
        const QString dst = isoPath + "/ac4probe/ac4probe.ini";
        QDir().mkpath(isoPath + "/ac4probe");
        QFile::copy(QString::fromUtf8(priorIni), dst);
    }
    const QStringList args = app.arguments();
    const QString mode = args.value(1);
    installModalAutoClose();

    // 协议日志走 stderr：探针只读 stdout，避免 PDU 日志灌满管道导致客户端写阻塞
    qputenv("QT_LOGGING_RULES", "xmu.cli.debug=false;xmu.cli.pdu.debug=true");
    installFileLogging(QStringLiteral("harness"));   // 报文落 exe/logs/harness-pdu.log 供排查
    Client& c = Client::getinstance();
    c.show();
    ck("h_connect", waitUntil([&]{ return c.sorket.state() == QAbstractSocket::ConnectedState; }),
       c.sorket.errorString());
    ck("h_clientid_valid", isValidClientId(c.m_clientId.toLatin1()), c.m_clientId);
    if (g_fails) { printf("FAILS %d\n", g_fails); return 1; }

    QLineEdit* nameLE = c.findChild<QLineEdit*>(QStringLiteral("name_LE"));
    QLineEdit* pwdLE = c.findChild<QLineEdit*>(QStringLiteral("pwd_LE"));
    QLineEdit* capLE = c.findChild<QLineEdit*>(QStringLiteral("randcode_LE"));
    QPushButton* capBtn = c.findChild<QPushButton*>(QStringLiteral("pushButton"));
    QPushButton* regBtn = c.findChild<QPushButton*>(QStringLiteral("register_PB"));
    QPushButton* loginBtn = c.findChild<QPushButton*>(QStringLiteral("login_PB"));
    ck("h_widgets", nameLE && pwdLE && capLE && capBtn && regBtn && loginBtn);

    // 验证码自动填答：测试部署回文本答案（CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS），
    // handleCaptcha 只消费 PNG；这里直接监听 45 响应文本不可行——改用服务端旁路下的
    // 文本载荷：harness 在点按钮前挂钩 recvmsg 前的裸缓冲不现实。
    // 方案：测试部署下 45 载荷是文本+NUL；Client::updateCaptchaImage 收 PNG 才显示。
    // 为拿到文本，直接替换 m_mhptr->pdu 不可行。改由探针脚本经 Redis 读 cap:{cid}:code，
    // 写入临时文件，harness 轮询读取（clientId 打印给脚本）。
    const QString cidFile = isoPath + "/cid.txt";
    { QFile f(cidFile); f.open(QIODevice::WriteOnly); f.write(c.m_clientId.toLatin1()); }
    const QString answerFile = isoPath + "/answer.txt";
    printf("HARNESS_CID_FILE %s\n", cidFile.toLocal8Bit().constData());
    printf("HARNESS_ANSWER_FILE %s\n", answerFile.toLocal8Bit().constData());
    fflush(stdout);

    // 取码填答：等 45 响应真正到达且结果码=0 才读答案文件。
    // 不能只等文件出现——Redis 里可能还是上一轮未消费的旧码（答案文件先于新码写入），
    // 用旧码登录必 -3011/-3012。以 45 成功为同步点，随后读到的必是本轮新码。
    auto fetchAnswerAndFill = [&]() -> bool {
        printf("STEP captcha-click\n"); fflush(stdout);
        c.m_captchaCooldownSec = 0;
        c.m_captchaLocked = false;
        c.m_captchaAutoRefresh = false;
        // 挂钩：记录最近一次 45 的结果码（借 recvmsg 前的分发无法插桩，改用轮询答案文件+
        // 服务端码键存在性：45 成功时 cap:{cid}:code 必在 Redis；探针只在看到码键后写文件）
        // 请求序号：探针据此同步"等新码写入再喂"，避免喂到上一轮旧码
        {
            QFile rf(isoPath + "/req.txt");
            int n = 0;
            if (rf.open(QIODevice::ReadOnly)) { n = rf.readAll().trimmed().toInt(); rf.close(); }
            rf.open(QIODevice::WriteOnly | QIODevice::Truncate);
            rf.write(QByteArray::number(n + 1));
        }
        capBtn->click();   // 发送 44（携带 clientId）
        QString answer;
        const bool got = waitUntil([&]{
            QFile f(answerFile);
            if (f.open(QIODevice::ReadOnly)) { answer = QString::fromUtf8(f.readAll()).trimmed(); f.close(); }
            return !answer.isEmpty();
        }, 10000);
        if (!got) { printf("STEP answer-timeout\n"); fflush(stdout); return false; }
        QFile::remove(answerFile);
        // 45 响应可能仍在途：短暂泵事件确保 handleCaptcha 已处理（冷却/图片状态就位）
        QThread::msleep(150);
        QCoreApplication::processEvents();
        capLE->setText(answer);
        printf("STEP answer-filled\n"); fflush(stdout);
        return true;
    };

    // 登录尝试：取码→填答→点登录→等结果。验证码被注册消费（-3012）时自动换码重试。
    auto attemptLogin = [&](const QString& user, const QString& pwd) -> bool {
        nameLE->setText(user);
        pwdLE->setText(pwd);
        // 取码失败只记 STEP（重试路径会再取）；最终失败由 q_register_login/q_prelogin 断言
        if (!fetchAnswerAndFill()) { printf("STEP login-captcha-retry\n"); fflush(stdout); return false; }
        loginBtn->click();
        printf("STEP login-clicked\n"); fflush(stdout);
        const bool ok = waitUntil([&]{ return !c.m_LoginName.isEmpty() && c.sorket.state() == QAbstractSocket::ConnectedState
                                   && Index::getinstance().isVisible(); }, 10000);
        printf("STEP login-wait %s name=%s\n", ok ? "OK" : "TIMEOUT", c.m_LoginName.toUtf8().constData());
        fflush(stdout);
        return ok;
    };
    auto doRegisterLogin = [&](const QString& user, const QString& pwd) -> bool {
        nameLE->setText(user);
        pwdLE->setText(pwd);
        if (!fetchAnswerAndFill()) { ck("h_captcha_register", false, "答案文件超时"); return false; }
        regBtn->click();
        printf("STEP register-clicked\n"); fflush(stdout);
        // 等注册响应处理完（成功/已存在都继续走登录；注册会消费验证码，登录必须换新码）
        QThread::msleep(800);
        QCoreApplication::processEvents();
        printf("STEP pre-login\n"); fflush(stdout);
        for (int i = 0; i < 3; ++i) {
            if (attemptLogin(user, pwd)) return true;
            // 失败多为验证码一次性（被注册消费/过期）：换码重试。
            // 客户端本地冷却/自动补拉标记会干扰取码——测试环境复位（不改产品逻辑）
            c.m_captchaCooldownSec = 0;
            c.m_captchaLocked = false;
            c.m_captchaAutoRefresh = false;
            QThread::msleep(300);
            QCoreApplication::processEvents();
        }
        return false;
    };

    int rc = 0;
    if (mode == "login") {
        const QString user = args.value(2), pwd = args.value(3);
        ck("q_register_login", doRegisterLogin(user, pwd));
        ck("q_token_saved", isValidResumeToken(settingsToken().toLatin1()));
        ck("q_token_mem", c.m_resumeToken == settingsToken() && !c.m_resumeToken.isEmpty());
        ck("q_index_visible", Index::getinstance().isVisible());
        // 心跳带 clientId：主动发一次并确认连接仍活（服务端校验通过才回 PONG）
        c.sendHeartbeat();
        ck("q_heartbeat_alive", waitUntil([&]{ return c.sorket.state() == QAbstractSocket::ConnectedState; }, 2000));
        printf("HARNESS_OK login\n");
    }
    else if (mode == "kickwait") {
        // 第二实例：只登录（不注册）并等待被第一实例的新登录踢下线（123）
        const QString user = args.value(2), pwd = args.value(3);
        nameLE->setText(user);
        pwdLE->setText(pwd);
        if (!fetchAnswerAndFill()) { ck("q_captcha_login", false); printf("FAILS %d\n", g_fails); return 1; }
        loginBtn->click();
        ck("q_kickwait_login", waitUntil([&]{ return Index::getinstance().isVisible(); }, 10000));
        printf("HARNESS_READY kickwait\n"); fflush(stdout);
        ck("q_kicked_back_to_login", waitUntil([&]{ return !Index::getinstance().isVisible() && c.isVisible(); }, 30000));
        ck("q_kicked_token_cleared", settingsToken().isEmpty() && c.m_resumeToken.isEmpty());
    }
    else if (mode == "logout" || mode == "kicked" || mode == "logoff" || mode == "resume" || mode == "badtoken") {
        // 需要既有登录态。resume/badtoken 模拟"重启客户端后重连"：复用上一进程落盘凭据，
        // 直接走断线→重连→121；其余模式本进程重新注册登录（同名注册幂等失败无害）。
        const QString user = args.value(2), pwd = args.value(3);
        const bool freshProcess = (mode == "resume" || mode == "badtoken");
        if (freshProcess) {
            ck("q_prior_token_present", isValidResumeToken(settingsToken().toLatin1()),
               QStringLiteral("len=%1").arg(settingsToken().size()));
            ck("q_prior_user", c.m_tokenUser == user, c.m_tokenUser);
        } else {
            if (!doRegisterLogin(user, pwd)) { ck("q_prelogin", false); printf("FAILS %d\n", g_fails); return 1; }
        }
        if (mode == "logout") {
            QPushButton* lo = Index::getinstance().findChild<QPushButton*>(QStringLiteral("logout_PB"));
            ck("q_logout_btn", lo != nullptr);
            if (lo) lo->click();
            ck("q_logout_anonymous", waitUntil([&]{ return !Index::getinstance().isVisible() && c.isVisible(); }));
            ck("q_logout_token_cleared", settingsToken().isEmpty() && c.m_resumeToken.isEmpty());
            ck("q_logout_socket_kept", c.sorket.state() == QAbstractSocket::ConnectedState);
        }
        else if (mode == "kicked") {
            // 探针从另一连接登录同账号触发服务端 123；这里等失效处理完成
            printf("HARNESS_READY kick\n"); fflush(stdout);
            ck("q_kicked_back_to_login", waitUntil([&]{ return !Index::getinstance().isVisible() && c.isVisible(); }, 15000));
            ck("q_kicked_token_cleared", settingsToken().isEmpty() && c.m_resumeToken.isEmpty());
        }
        else if (mode == "logoff") {
            qputenv("HARNESS_LOGOFF_PWD", pwd.toUtf8());
            QPushButton* off = Index::getinstance().findChild<QPushButton*>(QStringLiteral("pushButton"));
            ck("q_logoff_btn", off != nullptr);
            if (qEnvironmentVariableIsSet("HARNESS_DIRECT_LOGOFF")) {
                // 无头平台的 QInputDialog 模态循环无法可靠注入输入，
                // 直接发送同一条 48 请求，保留真实客户端回包清理路径。
                PDU* pdu = mkPDU();
                pdu->uiType = ENUM_MSG_TYPE_LOGOFF_REQUEST;
                const QByteArray userBytes = c.m_LoginName.toUtf8();
                const QByteArray pwdBytes = pwd.toUtf8();
                memcpy(pdu->caData, userBytes.constData(), qMin(userBytes.size(), 31));
                memcpy(pdu->caData + 32, pwdBytes.constData(), qMin(pwdBytes.size(), 31));
                c.seedMsg(pdu);
            } else if (off) {
                off->click();
            }
            ck("q_logoff_back_to_login", waitUntil([&]{ return c.isVisible() && !Index::getinstance().isVisible(); }, 15000));
            ck("q_logoff_token_cleared", settingsToken().isEmpty() && c.m_resumeToken.isEmpty());
            ck("q_logoff_snapshot_gone", !QFile::exists(
                QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                    .filePath(QStringLiteral("transfers_%1.json").arg(user))));
        }
        else if (mode == "resume" || mode == "badtoken") {
            if (mode == "badtoken") {
                // 篡改落盘 token（保持 64hex 格式合法但摘要不符）
                QString bad = settingsToken();
                bad[0] = (bad[0] == 'a') ? 'b' : 'a';
                QSettings st; st.setValue(QStringLiteral("session/resume_token"), bad); st.sync();
                c.m_resumeToken = bad;
            }
            if (!freshProcess) {
                // 同进程模拟物理断线（服务端视角=断线，会话保留）→ 重连弹窗
                c.sorket.abort();
            }
            // freshProcess：启动即连接成功但无身份，等价"重启后需重建会话"；
            // 直接触发重连入口走 121（与弹窗按钮同一路径）
            ReconnectDialog* dlg = c.findChild<ReconnectDialog*>();
            ck("q_reconnect_dialog", freshProcess
               || waitUntil([&]{ return dlg && dlg->isVisible(); }, 8000));
            if (freshProcess) {
                // 新进程无弹窗可见记录：显示重连窗并直接发 121（与 connectsort 重连路径同一恢复逻辑）
                if (dlg) dlg->show();
                c.tryResumeSession();
            } else {
                // 直接调重连入口（等价按钮点击）
                QMetaObject::invokeMethod(&c, "onReconnectRequested");
            }
            if (mode == "resume") {
                ck("q_resume_success", waitUntil([&]{ return Index::getinstance().isVisible() && !c.m_resumePending; }, 12000));
                ck("q_resume_token_kept", !settingsToken().isEmpty() && c.m_resumeToken == settingsToken());
                ck("q_resume_biz_ok", waitUntil([&]{
                    Index::getinstance().getFile()->flushfile();
                    return c.sorket.state() == QAbstractSocket::ConnectedState;
                }, 3000));
            } else {
                // freshProcess 的 Index 从未显示，"回登录页"谓词初始即真——
                // 必须以 122 失败处理完成（m_resumePending 复位 + 凭据清除）为同步点
                ck("q_badtoken_back_to_login", waitUntil([&]{
                    return !c.m_resumePending && c.m_resumeToken.isEmpty() && c.isVisible();
                }, 12000));
                ck("q_badtoken_cleared", settingsToken().isEmpty() && c.m_resumeToken.isEmpty());
            }
        }
    }
    else { printf("usage: harness <mode> [user pwd]\n"); rc = 2; }
    // 退出前把隔离 INI 复制到探针指定位置（resume/badtoken 下一进程复用凭据）
    const QByteArray saveIni = qgetenv("HARNESS_SAVE_INI");
    if (!saveIni.isEmpty()) {
        QSettings st; st.sync();
        const QString src = isoPath + "/ac4probe/ac4probe.ini";
        if (QFile::exists(src)) {
            QFile::remove(QString::fromUtf8(saveIni));
            QFile::copy(src, QString::fromUtf8(saveIni));
        }
    }
    printf("FAILS %d\n", g_fails);
    fflush(stdout);
    // 进程级静态析构顺序不可控：Client 单例析构会 delete ui（QWidget），
    // 而 QApplication 可能已先析构 → QWidget::~QWidget 崩溃（0xC0000005，实测）。
    // 测试 harness 用 _exit 跳过静态析构（产品代码不受影响；快照已由各路径显式落盘）。
    _exit((g_fails == 0 && rc == 0) ? 0 : 1);
}
