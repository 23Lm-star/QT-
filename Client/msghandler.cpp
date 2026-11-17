// 业务响应处理实现：读取 PDU 的 caData/caMsg，更新界面或回送新 PDU。
// 约定：ret 等整型字段直接 memcpy 前 4 字节；字符串字段为定长数组，读取时
// 先补 NUL 再构造 QString。所有弹窗/界面操作都在 GUI 线程。
#include "logging.h"
#include "client.h"
#include "msghandler.h"
#include "chatbounds.h"
#include <QString>
#include <QMessageBox>
#include <qmessagebox.h>
#include <qdir.h>
#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include "index.h"
MsgHandler::MsgHandler()
{
}

// 注册响应：caData[0..3]=ret，[32..63]=登录名。
void MsgHandler::handlrigist(){
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,this->pdu->caData,PROTOCOL_CODE_SIZE);
    qCDebug(lcCli)<<"rigist code"<<code<<endl;
    char rigistName[32]={'\0'};
    memcpy(rigistName,this->pdu->caData+32,32);
    qCDebug(lcCli)<<QString(rigistName)<<endl;
    if(code==ENUM_PROTOCOL_CODE_CAPTCHA_WRONG || code==ENUM_PROTOCOL_CODE_CAPTCHA_EXPIRED){
        QMessageBox::information(&Client::getinstance(),"提示",protocolCodeText(code));
        return;
    }
    if(code==ENUM_PROTOCOL_CODE_CAPTCHA_LOCKED){
        int rem=0;
        memcpy(&rem,this->pdu->caData+4,sizeof(rem));
        // 触发客户端 10 分钟锁定：获取验证码按钮进入倒计时，注册/登录被客户端拦截
        Client::getinstance().startCaptchaCooldown(rem > 0 ? rem : 600, true);
        QMessageBox::information(&Client::getinstance(),"提示",
            QString("验证码错误次数过多，账号已锁定，请 %1 分钟后再试").arg(qMax(1,rem/60)));
        return;
    }
    if(code==ENUM_PROTOCOL_CODE_OK){
        // 成功：延迟 1.5s 让动画走完，再提示并本地创建用户目录。
        Client::getinstance().showLoadingAnimation(true);
        QString name=QString(rigistName);
        QTimer::singleShot(1500,[name](){
            QMessageBox::information(&Client::getinstance(),"提示","注册成功");
            Client::getinstance().flus();
            // 本地也建一份目录（服务端存储根同步），失败无提示、不阻断流程。
            QDir dir;
            dir.mkdir(QString("%1").arg(Client::getinstance().m_strRootPath));
            dir.mkdir(QString("%1/%2").arg(Client::getinstance().m_strRootPath).arg(name));
        });
    }
    else {
        Client::getinstance().showLoadingAnimation(false);
        QMessageBox::information(&Client::getinstance(),"提示",protocolCodeText(code));
    }
}

// 登录响应：ret=1 成功（caData[32..63]=登录名），-1 账号/密码错误；
// 验证码错误/过期/锁定的处理与注册响应一致。成功时立即返回名字。
QString MsgHandler::handllogin()
{
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    if(code==ENUM_PROTOCOL_CODE_CAPTCHA_WRONG || code==ENUM_PROTOCOL_CODE_CAPTCHA_EXPIRED){
        QMessageBox::information(&Client::getinstance(),"提示",protocolCodeText(code));
        // 验证码一次性：失败后自动换新码，复用旧图必再失败（-3012，2026-11-14 GUI 实测）
        Client::getinstance().requestCaptchaAutoRefresh();
        return QString();
    }
    if(code==ENUM_PROTOCOL_CODE_CAPTCHA_LOCKED){
        int rem=0;
        memcpy(&rem,pdu->caData+4,sizeof(rem));
        // 触发客户端 10 分钟锁定：获取验证码按钮进入倒计时，注册/登录被客户端拦截
        Client::getinstance().startCaptchaCooldown(rem > 0 ? rem : 600, true);
        QMessageBox::information(&Client::getinstance(),"提示",
            QString("验证码错误次数过多，账号已锁定，请 %1 分钟后再试").arg(qMax(1,rem/60)));
        return QString();
    }
    if(code==ENUM_PROTOCOL_CODE_OK){
        // 成功：先同步取名字返回，UI 切换延后 1.5s。
        Client::getinstance().showLoadingAnimation(true);
        char lName[32]={'\0'};
        memcpy(lName,pdu->caData+32,sizeof(lName));
        QString loginName=QString(lName);
        // R-1：名字同步返回给调用方（Client::handlMsg 的 LOGIN_RESPOND 分支先落 m_LoginName
        // 再 setCurrentUser）；本定时器此后只负责 UI 切换与既有延迟刷新。
        QTimer::singleShot(1500,[loginName](){
            qCDebug(lcCli)<<"登录账号成功，用户名为:"<<loginName;
            // REQ-EXT-GROUP-004：新会话丢弃上一会话的群读状态（换账号登录不得继承旧游标）
            Client::getinstance().resetGroupStates();
            // 文件页的用户根按本次登录身份重算——重连路径下 File 可能先于登录被构造，
            // 那次构造拿到的空登录名会让文件页停在存储根目录
            Index::getinstance().getFile()->resetUserPath();
            Index::getinstance().show();
            Client::getinstance().hide();
            // 主界面完全构造并显示后，在事件循环下一轮触发初始刷新（仅好友列表）
            QTimer::singleShot(0, [](){
                Index::getinstance().getFriend()->flushfriend();
                // REQ-EXT-GROUP-004：这里是全客户端唯一的"会话已建立"时点，一次取回全部群的
                // 未读锚点（群水位 + 我的已读游标）。不能再早——actor 未建立，摘要必被拒；
                // 不能再晚——等到第一次打开群聊窗口才取，会让没打开过的群一直空着。
                Index::getinstance().getGroup()->initializeSession();
                // 文件列表刷新延迟到用户首次点击"文件"页时
            });
        });
        return loginName;
    }
    else{
        Client::getinstance().showLoadingAnimation(false);
        const QString text=protocolCodeText(code);
        QTimer::singleShot(1500,[text](){
            QMessageBox::information(&Client::getinstance(),"提示",text);
        });
    }
    return QString();
}

void MsgHandler::handlonlineuser(){
      // REQ-PAGE-FETCH-001：所有用户分页响应（caData 码/total/has_more/count，载荷每条 33B）
      uint total=0; quint8 hasMore=0; quint32 count=0;
      memcpy(&total,pdu->caData+ALLUSER_RESP_TOTAL_OFFSET,sizeof(total));
      memcpy(&hasMore,pdu->caData+ALLUSER_RESP_HASMORE_OFFSET,sizeof(hasMore));
      memcpy(&count,pdu->caData+ALLUSER_RESP_COUNT_OFFSET,sizeof(count));
      const uint cap=uint(pdu->uiMsgLen/ALLUSER_ENTRY_BYTES);
      if(count>cap) count=cap;
      QVector<QPair<QString,bool>> entries;
      entries.reserve(int(count));
      for(uint i=0;i<count;i++){
          const uint base=i*ALLUSER_ENTRY_BYTES;
          char userName[ALLUSER_ENTRY_NAME_BYTES]={'\0'};
          memcpy(userName,pdu->caMsg+base,ALLUSER_ENTRY_NAME_BYTES);
          const bool online=pdu->caMsg[base+ALLUSER_ENTRY_NAME_BYTES]==1;
          entries.append(qMakePair(QString::fromUtf8(userName),online));
      }
      Index::getinstance().getFriend()->m_onlineUser->updateOnlineUser(entries,total,hasMore==1,true);
}

void MsgHandler::handlAddFriend()
{
    int code=0;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    qCDebug(lcCli)<<"AddFriend ans code"<<code<<endl;
    // REQ-UX-001 A1：任何结果都必须有可见反馈并更新在线用户窗口提示条
    OnlineUser *ou=Index::getinstance().getFriend()->m_onlineUser;
    if(code==ENUM_PROTOCOL_CODE_OK){
        // ret=1 来自对方的"同意"回执：双向好友关系已建立
        Index::getinstance().getFriend()->flushfriend();
        if(ou){ ou->setHint(QStringLiteral("好友添加成功")); }
        QMessageBox::information(ou,"提示","好友添加成功");
    }
    else if(code==ENUM_PROTOCOL_CODE_ACCEPTED){
        // REQ-CHAT-ACK-001 R10：申请已登记（离线也可登记），等待对方下次上线/刷新处理
        if(ou){ ou->setHint(QStringLiteral("好友申请已发送，等待对方确认")); }
        QMessageBox::information(ou,"提示","好友申请已发送，等待对方确认");
    }
    else {
        const QString text=protocolCodeText(code);
        if(ou){ ou->setHint(text); }
        QMessageBox::information(ou,"提示",text);
    }
}
//同意添加好友
// 请求方名字在 caData[0..31]；同意回执按“我/对方”顺序各占 32 字节。
void MsgHandler::handlAddFriendAgree(const QString &myName)
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    int ret=QMessageBox::question(Index::getinstance().getFriend(),"提示",QString("你同意%1的好友申请吗?").arg(seedName));
    if(ret!=QMessageBox::Yes){
        return;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_RESPOND;
    memcpy(pdu->caData,myName.toStdString().c_str(),sizeof(myName.toStdString()));
    memcpy(pdu->caData+32,&seedName,sizeof (seedName));
    // REQ-EXT-TLS-001 业务规则 5：TLS 握手完成前不发 PDU，直接丢弃该尽力发送请求（重连后可再发）。
    if (Client::getinstance().tlsPending()) {
        free(pdu);
        return;
    }
    Client::getinstance().sorket.write((char*)pdu,pdu->uiTotalLen);
}

// 好友列表：caMsg 是 32 字节定长用户名数组，数量 = uiMsgLen/32。
void MsgHandler::handlFlushFriend()
{
    // REQ-PAGE-FETCH-001：好友列表服务端数量截断（至多 PAGE_MAX_LIMIT 条），超限提示
    quint8 truncated=0; quint32 count=0;
    memcpy(&truncated,pdu->caData+LIST_TRUNC_RESP_TRUNCATED_OFFSET,sizeof(truncated));
    memcpy(&count,pdu->caData+LIST_TRUNC_RESP_COUNT_OFFSET,sizeof(count));
    const uint cap=uint(pdu->uiMsgLen/32);
    if(count==0) count=cap;                        // 兼容空 caData（旧响应/测试桩）
    if(count>cap) count=cap;
    QStringList userNamelist;
    for(uint i=0;i<count;i++){
        char userName[32]={'\0'};
        memcpy(userName,pdu->caMsg+i*32,32);//取出每一个用户的用户名
        userNamelist.append(QString("%1.%2").arg(i+1).arg(userName));//将编号还有用户名都统一加进去
    }
    Index::getinstance().getFriend()->flushFriend_LW(userNamelist);
    if(truncated){
        QMessageBox::information(Index::getinstance().getFriend(),QStringLiteral("提示"),
            QStringLiteral("好友数量较多，已截断显示最近 %1 条").arg(uint(count)));
    }
}
//删除好友
void MsgHandler::handlDeleteFriend()
{
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    if(code==ENUM_PROTOCOL_CODE_OK){
        Index::getinstance().getFriend()->flushfriend();
    }
    else {
        QMessageBox::information(Index::getinstance().getFriend(),"提示",protocolCodeText(code));
    }
}


//聊天
// 收到的是单条聊天消息：caData 前 32 字节为发送者，caMsg 为一个 Msg 结构。
void MsgHandler::chat()
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    // REQ-CLI-MSGBND-001：改前按报文里的 msgLen 把正文拷进 char[110]，拷贝长度完全由对端决定；
    // 现在按报文实际携带的字节取用，声明越界时钳制并裁到最后一个完整码点。
    QByteArray body;
    QString currtime;
    if (!readChatMsg(pdu->caMsg, pdu->uiMsgLen, &body, &currtime)) return;
    // REQ-CLI-MSGSTORE-001：渲染与落库统一交给 Chat
    Chat* chat = Index::getinstance().getFriend()->m_chat;
    chat->m_strChatName = seedName;
    chat->receivePeerMessage(QString(seedName), QString::fromUtf8(body), currtime);
}
//获取所有未读消息
void MsgHandler::getcharmsg()
{
        QStringList Msglist;
        // REQ-CLI-MSGBND-001：连续 Msg 记录改为按 stride 步进并逐条校验。改前直接用报文里的
        // totalLen 推进（不校验是否越出报文，totalLen==0 即死循环），正文还拷进无钳制的 char[110]。
        for(uint dis=0; dis<pdu->uiMsgLen; ){
            const uint stride=chatMsgRecordStride(pdu->caMsg, pdu->uiMsgLen, dis);
            if(stride==0) break;                        // 畸形即止，与网页端 parseMsgRecords 同口径
            QByteArray body;
            QString currTime;
            if(!readChatMsg(pdu->caMsg+dis, stride, &body, &currTime)) break;
            Msglist.append(QString::fromUtf8(body));
            Msglist.append(currTime);
            qCDebug(lcCli)<<"getmsg msg:"<<QString::fromUtf8(body)<<" currTime:"<<currTime<<"  ";
            dis+=stride;
        }
        // REQ-CLI-MSGSTORE-001：18 是消费式离线补发（服务端只回未送达给本人的行，
        // 取后标记已送达），方向恒为接收；逐条走 receivePeerMessage 完成去重、渲染与落库
        Chat* chat = Index::getinstance().getFriend()->m_chat;
        // 归属以发请求时记录的会话为准（响应体里没有对方名）
        const QString peer = chat->historyPeer().isEmpty() ? chat->m_strChatName : chat->historyPeer();
        for (int i = 0; i + 1 < Msglist.size(); i += 2)
            chat->receivePeerMessage(peer, Msglist.at(i), Msglist.at(i + 1));
}

// REQ-CHAT-ACK-001：109 实时投递（caData[0,32)=发送人、[32,40)=消息 ID）。
// 渲染与落库后立刻回 110；落库失败只降级为内存展示，仍确认避免无限重投。
void MsgHandler::chatDeliver()
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    qint64 msgId=0;
    memcpy(&msgId,pdu->caData+32,sizeof(msgId));
    QByteArray body;
    QString currtime;
    if (!readChatMsg(pdu->caMsg, pdu->uiMsgLen, &body, &currtime)) return;
    Chat* chat = Index::getinstance().getFriend()->m_chat;
    chat->m_strChatName = seedName;
    chat->receivePeerMessage(QString(seedName), QString::fromUtf8(body), currtime);
    if (msgId > 0) {
        QList<qint64> ids;
        ids.append(msgId);
        sendChatDeliveredAck(ids);
    }
}

// REQ-CHAT-ACK-001：112 批量待送达记录。逐条严格边界校验，畸形即止；
// 全部渲染落库后一次性回 110（服务端按 ID+收件人置已送达）。
void MsgHandler::getPendingChat()
{
    Chat* chat = Index::getinstance().getFriend()->m_chat;
    const QString peer = chat->historyPeer().isEmpty() ? chat->m_strChatName : chat->historyPeer();
    QList<qint64> ids;
    uint dis = 0;
    while (dis + uint(sizeof(qint64) + sizeof(qint32) * 2) <= pdu->uiMsgLen) {
        qint64 id = 0;
        qint32 contentLen = 0;
        qint32 timeLen = 0;
        memcpy(&id, pdu->caMsg + dis, sizeof(id)); dis += sizeof(id);
        memcpy(&contentLen, pdu->caMsg + dis, sizeof(contentLen)); dis += sizeof(contentLen);
        memcpy(&timeLen, pdu->caMsg + dis, sizeof(timeLen)); dis += sizeof(timeLen);
        if (contentLen < 0 || timeLen < 0) break;
        if (dis + uint(contentLen) + uint(timeLen) > pdu->uiMsgLen) break;
        const QByteArray body(pdu->caMsg + dis, int(contentLen)); dis += uint(contentLen);
        const QByteArray timeBytes(pdu->caMsg + dis, int(timeLen)); dis += uint(timeLen);
        chat->receivePeerMessage(peer, QString::fromUtf8(body), QString::fromUtf8(timeBytes));
        if (id > 0) ids.append(id);
    }
    sendChatDeliveredAck(ids);
    // REQ-PAGE-FETCH-001：本批已达上限仍有未送达 → 确认（110）后同会话再拉下一批
    quint8 truncated = 0;
    memcpy(&truncated, pdu->caData + CHAT_PULL_RESP_TRUNCATED_OFFSET, sizeof(truncated));
    if (truncated == 1 && !ids.isEmpty() && !peer.isEmpty()) {
        PDU* again = mkPDU();
        again->uiType = ENUM_MSG_TYPE_CHAT_PULL_PENDING_REQUEST;
        const QByteArray self = Client::getinstance().m_LoginName.toUtf8();
        const QByteArray peerB = peer.toUtf8();
        memcpy(again->caData, self.constData(), size_t(qMin(self.size(), 31)));
        memcpy(again->caData + 32, peerB.constData(), size_t(qMin(peerB.size(), 31)));
        Client::getinstance().seedMsg(again);
    }
}

void MsgHandler::sendChatDeliveredAck(const QList<qint64> &ids)
{
    if (ids.isEmpty()) return;
    PDU* ack = mkPDU(uint(ids.size() * int(sizeof(qint64))));
    ack->uiType = ENUM_MSG_TYPE_CHAT_DELIVER_ACK;
    for (int i = 0; i < ids.size(); ++i) {
        memcpy(ack->caMsg + i * int(sizeof(qint64)), &ids.at(i), sizeof(qint64));
    }
    Client::getinstance().seedMsg(ack);
}

// ===== REQ-PAGE-FETCH-001：118 历史分页响应（归属按发请求时记录的会话） =====
void MsgHandler::chatHistoryResp()
{
    int code = ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code, pdu->caData + CHAT_HIST_RESP_RET_OFFSET, PROTOCOL_CODE_SIZE);
    if (code != ENUM_PROTOCOL_CODE_OK) {
        QMessageBox::information(&Index::getinstance(), QStringLiteral("提示"), protocolCodeText(code));
        return;
    }
    uint total = 0; quint8 hasMore = 0; qint64 oldestId = 0; quint32 count = 0;
    memcpy(&total, pdu->caData + CHAT_HIST_RESP_TOTAL_OFFSET, sizeof(total));
    memcpy(&hasMore, pdu->caData + CHAT_HIST_RESP_HASMORE_OFFSET, sizeof(hasMore));
    memcpy(&oldestId, pdu->caMsg + CHAT_HIST_RESP_OLDEST_OFFSET, sizeof(oldestId));
    memcpy(&count, pdu->caMsg + CHAT_HIST_RESP_COUNT_OFFSET, sizeof(count));
    // 记录区严格边界校验（与 112 同口径，畸形即止）
    QVector<Chat::ServerHistRow> rows;
    uint dis = CHAT_HIST_RESP_RECORDS_OFFSET;
    for (uint i = 0; i < count; ++i) {
        if (dis + uint(CHAT_HIST_REC_BODY_OFFSET) > pdu->uiMsgLen) break;
        qint64 id = 0; quint8 dir = 0; qint32 contentLen = 0, timeLen = 0;
        memcpy(&id, pdu->caMsg + dis + CHAT_HIST_REC_ID_OFFSET, sizeof(id));
        dir = quint8(pdu->caMsg[dis + CHAT_HIST_REC_DIR_OFFSET]);
        memcpy(&contentLen, pdu->caMsg + dis + CHAT_HIST_REC_CONTENTLEN_OFFSET, sizeof(contentLen));
        memcpy(&timeLen, pdu->caMsg + dis + CHAT_HIST_REC_TIMELEN_OFFSET, sizeof(timeLen));
        if (contentLen < 0 || timeLen < 0) break;
        if (dis + uint(CHAT_HIST_REC_BODY_OFFSET) + uint(contentLen) + uint(timeLen) > pdu->uiMsgLen) break;
        Chat::ServerHistRow row;
        row.id = id;
        row.fromSelf = (dir == 0);           // 0=查询者发送，1=对方发送
        row.content = QString::fromUtf8(pdu->caMsg + dis + CHAT_HIST_REC_BODY_OFFSET, int(contentLen));
        row.timeText = QString::fromUtf8(pdu->caMsg + dis + CHAT_HIST_REC_BODY_OFFSET + uint(contentLen), int(timeLen));
        dis += uint(CHAT_HIST_REC_BODY_OFFSET) + uint(contentLen) + uint(timeLen);
        rows.append(row);
    }
    Chat* chat = Index::getinstance().getFriend()->m_chat;
    const QString peer = chat->historyPeer().isEmpty() ? chat->m_strChatName : chat->historyPeer();
    chat->applyServerHistory(peer, oldestId, hasMore == 1, total, rows);
}

// REQ-CHAT-ACK-001 R10：114 待处理好友申请。逐条询问；同意走原 10（服务端按 pending 校验），
// 拒绝回 115 把该行置 rejected，避免每次刷新重复弹出。
void MsgHandler::handlePendingFriendList()
{
    // REQ-PAGE-FETCH-001：服务端只回最近 N 天、至多 200 条，超限先提示再逐条询问
    quint8 reqTruncated = 0;
    memcpy(&reqTruncated, pdu->caData + LIST_TRUNC_RESP_TRUNCATED_OFFSET, sizeof(reqTruncated));
    const int count = int(pdu->uiMsgLen / 32);
    Friend* fr = Index::getinstance().getFriend();
    if (reqTruncated == 1) {
        QMessageBox::information(fr, QStringLiteral("提示"),
                                 QStringLiteral("好友申请较多，仅显示最近 %1 条").arg(count));
    }
    const QByteArray self = Client::getinstance().m_LoginName.toUtf8();
    for (int i = 0; i < count; ++i) {
        char name[32] = {'\0'};
        memcpy(name, pdu->caMsg + i * 32, 32);
        const QString requester = QString::fromUtf8(name);
        if (requester.isEmpty()) continue;
        const QMessageBox::StandardButton answer = QMessageBox::question(
            fr, QStringLiteral("提示"),
            QStringLiteral("你同意%1的好友申请吗?").arg(requester),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        PDU* req = mkPDU();
        if (answer == QMessageBox::Yes) {
            req->uiType = ENUM_MSG_TYPE_ADDFRIEND_RESPOND;
            memcpy(req->caData, self.constData(), size_t(qMin(self.size(), 31)));
            memcpy(req->caData + 32, name, 32);
        } else {
            req->uiType = ENUM_MSG_TYPE_REJECT_FRIEND_REQUEST;
            memcpy(req->caData, name, 32);
        }
        // TLS 握手完成前不发 PDU，保持与其它尽力发送一致
        if (Client::getinstance().tlsPending()) {
            free(req);
            continue;
        }
        Client::getinstance().sorket.write((char*)req, req->uiTotalLen);
        free(req);
    }
}
//创建目录
void MsgHandler::mkdir()
{
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    qCDebug(lcCli)<<"mkdir code"<<code<<endl;
    if(code==ENUM_PROTOCOL_CODE_OK){
        QMessageBox::information(&Index::getinstance(),"提示","创建文件夹成功");
        Index::getinstance().getFile()->flushfile();
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示",protocolCodeText(code));
    }
}

// 删除响应：成功不本地改列表，而是重新请求 FLUSHFILE（以服务端为准）。
void MsgHandler::deldir()
{
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    qCDebug(lcCli)<<"删除文件结果"<<code<<endl;
    if(code==ENUM_PROTOCOL_CODE_OK){
        QMessageBox::information(&Index::getinstance(),"提示","删除文件夹成功");
        // 注意：局部 pdu 覆盖成员指针，避免误用刚处理完的响应。
        PDU* pdu=mkPDU(Index::getinstance().getFile()->m_strCurPath.size()+1);
        pdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_REQUEST;
        memcpy(pdu->caMsg,Index::getinstance().getFile()->m_strCurPath.toStdString().c_str(),Index::getinstance().getFile()->m_strCurPath.size());
        Client::getinstance().seedMsg(pdu);
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示",protocolCodeText(code));
    }
}

// 切换目录响应：成功时 caMsg 为服务端确认后的相对路径。
void MsgHandler::changedir()
{
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    if(code==ENUM_PROTOCOL_CODE_OK){//如果路径存在
        Index::getinstance().getFile()->m_strCurPath=pdu->caMsg;
        Index::getinstance().getFile()->flushfile();
    }
    else {//如果路径不存在
        QMessageBox::information(&Index::getinstance(),"提示",protocolCodeText(code));
        Index::getinstance().getFile()->flushCurrLE();
    }
}
//重命名文件
void MsgHandler::renamedir()
{
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    qCDebug(lcCli)<<"rename code"<<code<<endl;
    if(code==ENUM_PROTOCOL_CODE_OK){
        QMessageBox::information(&Index::getinstance(),"提示","文件重命名成功");
        // 统一走 File::flushfile()：重置分页状态；手工拼请求不重置 m_loadedCount，响应被按「加载更多」追加，表现为改名后旧名+新名并存（2026-11-14 GUI 实测）
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示",protocolCodeText(code));
    }
}

void MsgHandler::sharefileagree()
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    QString filePath=pdu->caMsg;
    int index=filePath.lastIndexOf('/');
    QString fileName=filePath.right(filePath.size()-index-1);
    qCDebug(lcCli)<<"接收到seedName:"<<seedName<<"的文件fileName "<<fileName<<" ";
    int ret=QMessageBox::question(&Index::getinstance(),"分享文件请求",QString("你是否同意接受%1的文件%2").arg(seedName).arg(fileName));
    qCDebug(lcCli)<<" ret="<<ret<<endl;
    if(ret!=QMessageBox::Yes){
        return ;
    }
    PDU* respdu=mkPDU(pdu->uiMsgLen);
    respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND;
    memcpy(respdu->caData,Client::getinstance().m_LoginName.toStdString().c_str(),32);
    memcpy(respdu->caMsg,pdu->caMsg,respdu->uiMsgLen);
    Client::getinstance().seedMsg(respdu);
}

void MsgHandler::sharefilefinsh()
{
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    qCDebug(lcCli)<<"sharefilefinsh code"<<code<<endl;
    if(code==ENUM_PROTOCOL_CODE_OK){
        QMessageBox::information(&Index::getinstance(),"提示","接收分享文件成功");
        Index::getinstance().getFile()->flushfile();//刷新一下文件
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示",protocolCodeText(code));
    }
}

// 文件列表响应：caMsg 为连续的 FileInfo（caName[32] + uiType）。
// 这里为每条记录 new 一个副本，所有权随列表交给文件页刷新函数。
void MsgHandler::flushfile()
{
    // REQ-AUDIT-LOG-001：先看结果码——目录不存在/无权访问不再静默清空列表
    int listCode = ENUM_PROTOCOL_CODE_OK;
    memcpy(&listCode, pdu->caData + FLUSHFILE_RESP_RET_OFFSET, PROTOCOL_CODE_SIZE);
    // REQ-PAGE-FETCH-001：目录分页响应（caData 码/total/has_more/count，载荷 FileInfo 数组）
    uint total=0; quint8 hasMore=0; quint32 count=0;
    memcpy(&total,pdu->caData+FLUSHFILE_RESP_TOTAL_OFFSET,sizeof(total));
    memcpy(&hasMore,pdu->caData+FLUSHFILE_RESP_HASMORE_OFFSET,sizeof(hasMore));
    memcpy(&count,pdu->caData+FLUSHFILE_RESP_COUNT_OFFSET,sizeof(count));
    const uint cap=uint(pdu->uiMsgLen/sizeof(FileInfo));
    if(count==0) count=cap;                        // 兼容空 caData（旧响应/测试桩）
    if(count>cap) count=cap;
    QList<FileInfo*>qFileinfolist;
    for(uint i=0;i<count;i++){
        FileInfo* newFileInfo=new FileInfo;
        memcpy(newFileInfo,pdu->caMsg+i*sizeof (FileInfo),sizeof(FileInfo));
        qFileinfolist.append(newFileInfo);
    }
    if (listCode != ENUM_PROTOCOL_CODE_OK) {
        // 拒绝（如 -2020 无权访问/-4004 目录不存在）：提示且不清空现有列表，避免用户误以为目录为空
        for (FileInfo *p : qFileinfolist) delete p;
        QMessageBox::warning(Index::getinstance().getFile(), QStringLiteral("提示"),
                             protocolCodeText(listCode));
        return;
    }
    if (listCode != ENUM_PROTOCOL_CODE_OK) {
        // 拒绝（如 -2020 无权访问/-4004 目录不存在）：提示且不清空现有列表，避免用户误以为目录为空
        for (FileInfo *p : qFileinfolist) delete p;
        QMessageBox::warning(Index::getinstance().getFile(), QStringLiteral("提示"),
                             protocolCodeText(listCode));
        return;
    }
    // append=true：加载更多在已有列表后追加；第 0 页由 File 在请求时清空状态
    Index::getinstance().getFile()->updateCurrfilePage(qFileinfolist,total,hasMore==1);
}

// 解析分享链接响应：服务端已把文件复制到其【用户目录/分享链接/】，这里提示并刷新列表
void MsgHandler::handleResolvePublishLink(PDU* pdu)
{
    int code = ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code, pdu->caData, PROTOCOL_CODE_SIZE);
    qCDebug(lcCli) << "[handleResolvePublishLink] 服务端返回 code=" << code;
    if(code != ENUM_PROTOCOL_CODE_OK){
        QMessageBox::information(&Index::getinstance(), "提示", protocolCodeText(code));
        return;
    }
    // 服务端已把文件复制到其【用户目录/分享链接/】下，这里解析返回的相对路径并提示
    // 响应体是 JSON 文本；这里只做最小字符串查找，避免为一个字段引入 JSON 依赖。
    QString json = QString::fromUtf8(pdu->caMsg);
    QString savedPath;
    const QString key = "\"saved_path\":\"";
    int idx = json.indexOf(key);
    if(idx >= 0){
        int start = idx + key.length();
        int end = json.indexOf('"', start);
        if(end > start){
            savedPath = json.mid(start, end - start);
        }
    }
    if(savedPath.isEmpty()){
        QMessageBox::information(&Index::getinstance(), "提示", "获取分享文件失败：未返回保存路径");
        return;
    }
    QMessageBox::information(&Index::getinstance(), "获取成功",
                             QString("文件已保存到：%1").arg(savedPath));
    // 刷新文件列表，使“分享链接”文件夹出现在用户目录中
    Index::getinstance().getFile()->flushfile();
}

void MsgHandler::handlfinduser(){
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    int online=0;
    memcpy(&online,pdu->caData+PROTOCOL_CODE_SIZE,sizeof(online));
    qCDebug(lcCli)<<"client finduser code"<<code<<" online"<<online;
    if(code!=ENUM_PROTOCOL_CODE_OK){
        QMessageBox::information(&Client::getinstance(),"提示",protocolCodeText(code));
    }
    else if(online){
        QMessageBox::information(&Client::getinstance(),"提示","该用户在线");
    }
    else{
       QMessageBox::information(&Client::getinstance(),"提示","该用户不在线");
    }
}

// 验证码响应：caData[0..3]=status，[4..7]=服务端剩余秒数；成功时 caMsg=验证码文本。
void MsgHandler::handleCaptcha(){
    int code;
    memcpy(&code, pdu->caData, PROTOCOL_CODE_SIZE);
    int remaining;
    memcpy(&remaining, pdu->caData + 4, sizeof(remaining));

    if (code == ENUM_PROTOCOL_CODE_OK) {
        // 成功：REQ-AUDIT-001 R8——载荷是 PNG 图片，文本答案只在服务端
        const QByteArray png(pdu->caMsg, pdu->uiMsgLen);
        qCDebug(lcCli) << "收到服务端验证码图片，字节数:" << png.size();
        Client::getinstance().updateCaptchaImage(png);
        Client::getinstance().m_captchaAutoRefresh = false;   // 新码已到，自动补拉完成
        Client::getinstance().startCaptchaCooldown(remaining > 0 ? remaining : 60);
    } else if (code == ENUM_PROTOCOL_CODE_CAPTCHA_COOLDOWN) {
        // 冷却中：禁止获取，按服务端剩余秒数倒计时
        Client::getinstance().startCaptchaCooldown(remaining > 0 ? remaining : 60);
        QMessageBox::information(&Client::getinstance(), "提示",
            QString("获取验证码过于频繁，请 %1 秒后再试").arg(remaining));
    } else if (code == ENUM_PROTOCOL_CODE_CAPTCHA_LOCKED) {
        // 锁定中：获取验证码按钮进入 10 分钟倒计时，并禁用注册/登录（客户端拦截）
        Client::getinstance().startCaptchaCooldown(remaining > 0 ? remaining : 600, true);
        QMessageBox::information(&Client::getinstance(), "提示",
            QString("账号已锁定，请 %1 分钟后再试").arg(qMax(1, remaining / 60)));
    }
}

// 移动文件响应：caData[0]=结果，[32..63]=被移动文件名（用于提示）。
void MsgHandler::handlMoveFile()
{
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    char fileName[32]={' '};
    memcpy(fileName,pdu->caData+32,32);
    qCDebug(lcCli)<<"movefile code:"<<code<<" fileName:"<<QString::fromUtf8(fileName);
    if(code==ENUM_PROTOCOL_CODE_OK){
        QMessageBox::information(&Index::getinstance(),"提示",QString("文件%1移动成功").arg(QString::fromUtf8(fileName)));
        Index::getinstance().getFile()->flushfile();
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示",protocolCodeText(code));
    }
}

void MsgHandler::handlLogoff()
{
    int code=ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code,pdu->caData,PROTOCOL_CODE_SIZE);
    qCDebug(lcCli)<<"handlLogoff code:"<<code;
    if(code==ENUM_PROTOCOL_CODE_OK){
        QMessageBox::information(&Index::getinstance(),"提示","账户已成功注销，即将返回登录界面。");
        // REQ-CLI-MSGSTORE-001：账号注销后本地消息（含水位）一并清除，同名重注册不复用
        Client::getinstance().msgStore().removeUser();
        // 关闭主界面，返回登录界面
        Index::getinstance().hide();
        Client::getinstance().show();
        Client::getinstance().flus();
        // 断开连接
        Client::getinstance().sorket.disconnectFromHost();
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示",protocolCodeText(code));
    }
}


// REQ-CLI-MSGSTORE-001：服务端 16 应答（发送方专属）。
// 布局：caData[0,32)=对方名，[32)=bool 服务端入库结果；caMsg 原样回带本次发送的 Msg。
// 只有 stored=true 才把自发消息写进本地存储；报文不完整时按「结果未知」丢弃，不写库。
void MsgHandler::handleChatAck()
{
    if (pdu->uiMsgLen < sizeof(Msg)) return;
    char peerName[33] = {'\0'};
    memcpy(peerName, pdu->caData, 32);
    int code = ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code, pdu->caData + 32, PROTOCOL_CODE_SIZE);

    // REQ-CLI-MSGBND-001：正文与时间统一走 readChatMsg：按报文实际携带的字节取，
    // 声明越界时钳制并裁到完整码点。旧写法钳到 109 字节是迁就定长缓冲的历史惯怛。
    QByteArray body;
    QString currtime;
    if (!readChatMsg(pdu->caMsg, pdu->uiMsgLen, &body, &currtime)) return;
    Index::getinstance().getFriend()->m_chat->handleSendAck(
        QString::fromUtf8(peerName), QString::fromUtf8(body), currtime, code == ENUM_PROTOCOL_CODE_OK);
}

void MsgHandler::handleQueryFilePublishLink(PDU* pdu) {
    int code = ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code, pdu->caData, PROTOCOL_CODE_SIZE);

    if (code == ENUM_PROTOCOL_CODE_LINK_INVALID) {
        QMessageBox::information(&Index::getinstance(), "提示", "暂无分享链接");
        return;
    }
    // REQ-EXT-SHARE-001：链接已过期/已撤销时给出「重新生成 / 续期」的选择，
    // 不再把一条已知失效的链接当有效链接展示给主人
    if (code != ENUM_PROTOCOL_CODE_OK) {
        Index::getinstance().getFile()->promptShareLinkRecovery(code);
        return;
    }

    QString link = QString::fromUtf8(pdu->caMsg);
    // 需要根据当前上下文打开对话框
    // 假设 Index 有当前选中的文件路径
    QString filePath = Index::getinstance().getFile()->m_strCurPath + "/" + Index::getinstance().getFile()->m_strShareFileName;
    // 模态展示后延迟销毁：exec 返回时仍处于本函数栈内，deleteLater 更安全。
    ShareLinkDialog *dlg = new ShareLinkDialog(filePath, &Index::getinstance());
    dlg->setLink(link);
    dlg->exec();
    dlg->deleteLater();
}

void MsgHandler::handleCreatePublishLink(PDU* pdu) {
    int code = ENUM_PROTOCOL_CODE_INTERNAL;
    memcpy(&code, pdu->caData, PROTOCOL_CODE_SIZE);
    QString link = QString::fromUtf8(pdu->caMsg);
    if (code == ENUM_PROTOCOL_CODE_OK) {
        QMessageBox::information(&Index::getinstance(), "提示",
            QString("生成成功！\n分享链接：%1").arg(link));
    } else if (code == ENUM_PROTOCOL_CODE_LINK_EXPIRED || code == ENUM_PROTOCOL_CODE_LINK_REVOKED) {
        // 复用了一条已失效的链接，或续期被拒（例如链接已被撤销）→ 回到选择框
        Index::getinstance().getFile()->promptShareLinkRecovery(
            code, QStringLiteral("（本次操作没有拿到可用链接）"));
    } else {
        QMessageBox::information(&Index::getinstance(), "提示", protocolCodeText(code));
    }
}
