#include "logging.h"
#include "friend.h"
#include "protocol.h"
#include "ui_friend.h"
#include "client.h"
#include <QInputDialog>
#include <QMessageBox>
#include <qdebug.h>

// 好友页实现：所有操作都走“构造 PDU → Client::seedMsg”的异步模型，
// 结果回包由 MsgHandler 分发到 flushFriend_LW / 在线用户窗口等入口。
// 列表项文本约定为“序号.用户名”，删除/私聊时按首个 '.' 截取真实用户名。
Friend::Friend(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Friend)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
    // 两个子窗口均以 Friend 为父对象，随本页一起销毁
    m_onlineUser=new OnlineUser;
    m_chat=new Chat;
}

Friend::~Friend()
{
    delete ui;
    delete m_onlineUser;
    delete m_chat;
}

void Friend::flushfriend()
{
    // REQ-PAGE-FETCH-002：从第 0 页重置重拉（刷新/删除好友/加好友成功同此入口，避免 offset 漂移）
    m_friendList.clear();
    m_friendTotal=0; m_friendHasMore=false; m_friendCapped=false;
    rebuildFriendList();
    ui->loadMore_PB->setHidden(true);
    requestFriendPage(0);
    // REQ-CHAT-ACK-001 R10：顺带拉取仍待处理的好友申请（含离线期间登记的）
    requestPendingFriendRequests();
}

// REQ-PAGE-FETCH-002：好友分页请求（limit=50，offset=已加载条数）
void Friend::requestFriendPage(uint offset)
{
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST;
    const uint limit=50;
    memcpy(pdu->caData+FRIEND_REQ_LIMIT_OFFSET,&limit,sizeof(limit));
    memcpy(pdu->caData+FRIEND_REQ_OFFSET_OFFSET,&offset,sizeof(offset));
    m_friendInFlight=true;
    Client::getinstance().seedMsg(pdu);
}

void Friend::on_loadMore_PB_clicked()
{
    if(!m_friendHasMore || m_friendInFlight) return;
    requestFriendPage(uint(m_friendList.size()));
}

// REQ-CHAT-ACK-001 R10：拉取本人待处理的好友申请，响应 114 由 MsgHandler 逐条询问
void Friend::requestPendingFriendRequests()
{
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_LIST_PENDING_FRIEND_REQUEST;
    Client::getinstance().seedMsg(pdu);
}

QListWidget *Friend::getfile_LW()
{
    // 仅供需要直接操作控件的场景使用，数据刷新请优先走 flushFriend_LW
    return  ui->listWidget;
}

// flushFriend_LW：仅接管好友列表数据，不直接写 QListWidget
// REQ-PAGE-FETCH-002：追加本页纯用户名并跨页连续编号；封顶提示只在首页弹一次
void Friend::flushFriend_LW(const QStringList& q, uint total, bool hasMore, bool capped)
{
    qCDebug(lcCli) << "[Friend] flushFriend_LW: 追加" << q.size() << "个好友条目，total" << total << "hasMore" << hasMore;
    const bool firstPage = m_friendList.isEmpty();
    for(const QString& name : q){
        m_friendList.append(QString("%1.%2").arg(m_friendList.size()+1).arg(name));
    }
    m_friendTotal=total;
    m_friendHasMore=hasMore;
    m_friendCapped=capped;
    m_friendInFlight=false;
    ui->loadMore_PB->setHidden(!hasMore);
    m_pendingUiRefresh = true;
    scheduleUiRefresh();
    if(capped && firstPage){
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("好友总数超过 %1 条，仅显示前 %1 条").arg(LIST_TOTAL_CAP));
    }
}

// scheduleUiRefresh：合并排队——最多排入一个 UI 刷新回调
void Friend::scheduleUiRefresh()
{
    // 无待处理数据则跳过
    if (!m_pendingUiRefresh) {
        return;
    }
    // 不可见时不排队，等 showEvent 触发
    if (!isVisible()) {
        qCDebug(lcCli) << "[Friend] scheduleUiRefresh: 页面不可见，延迟到 showEvent";
        return;
    }
    // 已排队则跳过（合并去重）
    if (m_uiRefreshQueued) {
        return;
    }
    m_uiRefreshQueued = true;
    qCDebug(lcCli) << "[Friend] scheduleUiRefresh: 排入一次 UI 刷新";
    QTimer::singleShot(0, this, [this]() {
        m_uiRefreshQueued = false;
        // 回调执行时重新检查可见性与待刷新标志
        if (isVisible() && m_pendingUiRefresh) {
            qCDebug(lcCli) << "[Friend] UI 刷新：开始重建";
            rebuildFriendList();
            qCDebug(lcCli) << "[Friend] UI 刷新：完成";
        }
    });
}

// rebuildFriendList：唯一可写 QListWidget 的位置
void Friend::rebuildFriendList()
{
    ui->listWidget->clear();
    ui->listWidget->addItems(m_friendList);
    m_pendingUiRefresh = false;
}

void Friend::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    qCDebug(lcCli) << "[Friend] showEvent: 页面变为可见，pending=" << m_pendingUiRefresh;
    // 仅在页面可见后请求合并排队重建
    scheduleUiRefresh();
}

void Friend::on_findUser_PB_clicked()
{
    // 输入框取消时返回空串，直接放弃请求
    QString strname=QInputDialog::getText(this,"查找用户","用户名");
    qCDebug(lcCli)<<strname<<endl;
    if(strname==nullptr){
        return ;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_findUser_REQUEST;
    // caData 前 32 字节：待查找的用户名（REQ-AUDIT-FULL-001：按实际长度拷贝，防短源越界读）
    const QByteArray findBytes=strname.toUtf8();
    memcpy(pdu->caData,findBytes.constData(),size_t(qMin(findBytes.size(),31)));
    Client::getinstance().seedMsg(pdu);
}

void Friend::on_onlineUser_PB_clicked()
{
    // 先展示窗口再异步拉取列表（REQ-PAGE-FETCH-001：从第 0 页开始分页）
    if(m_onlineUser->isHidden()){
        m_onlineUser->show();
    }
    m_onlineUser->resetAndRequest();
}

void Friend::on_flush_PB_clicked()
{
   flushfriend();
}

void Friend::on_del_PB_clicked()
{
    QListWidgetItem* tar =ui->listWidget->currentItem();
    if(!tar){
        return ;
    }
    // 列表项形如“序号.用户名”，截掉首个 '.' 及之前的序号
    QString tarname=tar->text();
    int cnt=0;
    for(int i=0;i<tarname.size();i++){
        if(tarname[i]=='.'){
            cnt=i;break;
        }
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_DELETEFRIEND_REQUEST;
    // caData[0,32)=被删好友；[32,64)=发起删除的当前用户（REQ-AUDIT-FULL-001：按实际长度拷贝）
    const QByteArray tarBytes=tarname.mid(cnt+1).toUtf8();
    memcpy(pdu->caData,tarBytes.constData(),size_t(qMin(tarBytes.size(),31)));
    const QByteArray selfBytes=Client::getinstance().m_LoginName.toUtf8();
    memcpy(pdu->caData+32,selfBytes.constData(),size_t(qMin(selfBytes.size(),31)));
    Client::getinstance().seedMsg(pdu);
}

void Friend::on_chat_PB_clicked()
{
    QListWidgetItem* pitem=ui->listWidget->currentItem();//获取当前聊天的对象
    if(pitem==nullptr){//如果聊天对象为nullptr，那么就不能进行聊天
        return;
    }
    //取出聊天对象的名字（去除前面的序号）
    char pName[32]={'\0'};
    int cnt=0;
    for(int i=0;i<pitem->text().size();i++){
        if(pitem->text()[i]=='.'){
            cnt=i;
            break;
        }
    }
    // REQ-AUDIT-FULL-001：聊天对象按实际长度拷入（pName 已零初始化，≤31 保留 NUL）
    const QByteArray chatBytes=pitem->text().mid(cnt+1).toUtf8();
    memcpy(pName,chatBytes.constData(),size_t(qMin(chatBytes.size(),31)));
    m_chat->m_strChatName=pName;//记录聊天对象
    // 仅首次打开时拉取历史消息；窗口已显示则视为切换会话外的重复点击，直接忽略
    if(m_chat->isHidden()){
        m_chat->show();
        m_chat->setWindowTitle(m_chat->m_strChatName);
        // REQ-CLI-MSGSTORE-001：先把本机存过的历史灌进窗口，再发 17 取离线补发；
        // 两条路径都按「时间+正文」去重，同一条消息不会显示两遍
        m_chat->loadLocalHistory(pName);
        m_chat->beginHistoryRequest(m_chat->m_strChatName);   // 112 的归属按发请求时的会话定
        m_chat->requestServerHistory(0);                      // REQ-PAGE-FETCH-001：同时取服务端最新一页历史
        PDU* pdu= mkPDU();
        // REQ-CHAT-ACK-001：桌面端改走 111，响应 112 每条带消息 ID，落库后回 110 才置已送达
        pdu->uiType=ENUM_MSG_TYPE_CHAT_PULL_PENDING_REQUEST;
        // caData[0,32)=对方账号：服务端 111 以连接身份作收件人，仅从首段取对方（与 17 不同，探针/112 处理器同此口径）
        // 此前误按 17 布局把本人放首段，服务端变成「自己查自己」，112 恒空，离线补收失效（2026-11-14 GUI 实测）
        const QByteArray peerBytes=m_chat->m_strChatName.toUtf8();
        memcpy(pdu->caData,peerBytes.constData(),size_t(qMin(peerBytes.size(),31)));
        Client::getinstance().seedMsg(pdu);
    }
}
