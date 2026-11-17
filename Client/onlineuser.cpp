#include "logging.h"
#include "onlineuser.h"
#include "ui_onlineuser.h"

#include <QMessageBox>
#include <cstring>

namespace {
// 写入协议的定长字符串槽：先清零，最多拷 dstSize-1 字节以保留结尾 '\0'
void copyFixedField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize - 1));
}
}

OnlineUser::OnlineUser(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OnlineUser)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
    // 初始引导文案；列表刷新/单击/发送后会被覆盖
    ui->hintLabel->setText(QStringLiteral("单击选中，双击添加好友"));
    ui->loadMore_PB->setHidden(true);              // 有下一页时才显示
}

OnlineUser::~OnlineUser()
{
    delete ui;
}

void OnlineUser::setHint(const QString &text)
{
    ui->hintLabel->setText(text);
}

void OnlineUser::updateOnlineUser(const QVector<QPair<QString,bool>>& entries, uint total, bool hasMore, bool append, bool capped)
{
    // REQ-PAGE-FETCH-001：append=false 整页替换，append=true 追加下一页
    if(!append){
        ui->listWidget->clear();
        m_offset=0;
    }
    for(int i=0;i<entries.size();++i){
        const QString &name=entries[i].first;
        QListWidgetItem *item=new QListWidgetItem(ui->listWidget);
        item->setText(QStringLiteral("%1（%2）").arg(name, entries[i].second?QStringLiteral("在线"):QStringLiteral("离线")));
        item->setData(Qt::UserRole, name);         // 纯用户名存 UserRole，点击消费端按此取
    }
    m_offset+=uint(entries.size());
    m_total=total;
    m_hasMore=hasMore;
    m_capped=capped;
    m_inFlight=false;
    ui->loadMore_PB->setHidden(!hasMore);
    if(m_offset==0){
        ui->hintLabel->setText(m_keyword.isEmpty()?QStringLiteral("当前没有可添加的用户")
                                                 :QStringLiteral("没有匹配的用户"));
    }else{
        // REQ-PAGE-FETCH-002：提示条带搜索词与总量封顶说明
        QString hint=QStringLiteral("已显示 %1/%2 人 · 单击选中，双击添加好友").arg(m_offset).arg(m_total);
        if(!m_keyword.isEmpty()) hint+=QStringLiteral(" · 搜索：%1").arg(m_keyword);
        if(m_capped) hint+=QStringLiteral(" · 结果超过 %1 条，仅显示前 %1 条").arg(LIST_TOTAL_CAP);
        ui->hintLabel->setText(hint);
    }
}

// REQ-PAGE-FETCH-002：按 limit=50 与当前搜索词请求指定偏移页（关键词槽 caData[8,40)）
void OnlineUser::requestPage(uint offset)
{
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ONLINEUSER_REQUEST;
    const uint limit=50;
    memcpy(pdu->caData+ALLUSER_REQ_LIMIT_OFFSET,&limit,sizeof(limit));
    memcpy(pdu->caData+ALLUSER_REQ_OFFSET_OFFSET,&offset,sizeof(offset));
    copyFixedField(pdu->caData+ALLUSER_REQ_KEYWORD_OFFSET,ALLUSER_REQ_KEYWORD_BYTES,m_keyword.toUtf8());
    m_inFlight=true;
    Client::getinstance().seedMsg(pdu);
}

// 从第 0 页重新拉取：清空已加载状态与搜索词再发 7
void OnlineUser::resetAndRequest()
{
    ui->listWidget->clear();                       // 刷新入口即清空旧列表，响应统一追加
    ui->search_LE->clear();
    m_keyword.clear();
    m_offset=0; m_total=0; m_hasMore=false; m_capped=false;
    requestPage(0);
}

// 加载更多：按已加载条数请求下一页（携带当前搜索词）
void OnlineUser::on_loadMore_PB_clicked()
{
    if(!m_hasMore || m_inFlight) return;
    requestPage(m_offset);
}

// REQ-PAGE-FETCH-002：搜索/清空关键词均从第 0 页重拉；超限关键词客户端拦截不发送
void OnlineUser::on_search_PB_clicked()
{
    if(m_inFlight) return;   // 在途拦截：防连续搜索响应交错混页（重开窗口走 resetAndRequest 强制恢复）
    const QString kw=ui->search_LE->text().trimmed();
    if(kw.toUtf8().size()>ALLUSER_REQ_KEYWORD_BYTES-1){
        ui->hintLabel->setText(QStringLiteral("关键词过长（UTF-8 须 ≤31 字节），请缩短后重试"));
        return;
    }
    m_keyword=kw;
    ui->listWidget->clear();
    m_offset=0; m_total=0; m_hasMore=false; m_capped=false;
    requestPage(0);
}

void OnlineUser::on_search_LE_returnPressed()
{
    on_search_PB_clicked();
}

// REQ-UX-001 A1：单击必须有可见反馈（旧实现只有双击槽，单击完全无响应）
void OnlineUser::on_listWidget_itemClicked(QListWidgetItem *item)
{
    if(item == nullptr){
        return;
    }
    const QString name=item->data(Qt::UserRole).toString();
    ui->hintLabel->setText(QStringLiteral("已选中：%1　·　双击添加好友").arg(name.isEmpty()?item->text():name));
}

//在线用户 双击添加好友（先确认再发送，并给出可见结果反馈）
void OnlineUser::on_listWidget_itemDoubleClicked(QListWidgetItem *item)
{
    if(item == nullptr){
        return;
    }
    // REQ-PAGE-FETCH-001：纯用户名存 Qt::UserRole（显示文本附在线状态后缀）
    QString target = item->data(Qt::UserRole).toString();
    if(target.isEmpty()) target=item->text().trimmed();
    if(target.isEmpty()){
        return;
    }
    // 客户端先行拦截“加自己”；其余合法性仍由服务端判定
    const QString myName=Client::getinstance().m_LoginName;
    if(target == myName){
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("不能添加自己为好友"));
        return;
    }
    if(QMessageBox::question(this, QStringLiteral("添加好友"),
                             QStringLiteral("是否添加 %1 为好友？").arg(target),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes){
        return;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_REQUEST;
    // caData[0,32)=申请人；[32,64)=被添加者
    copyFixedField(pdu->caData,32,myName.toUtf8());
    copyFixedField(pdu->caData+32,32,target.toUtf8());
    Client::getinstance().seedMsg(pdu);
    // REQ-UX-001 A1：立即反馈用提示条即可；弹窗由 ADDFRIEND_RESPOND 处理统一出，
    // 点击与应答各弹一次会双重提示（2026-11-14 GUI 实测）
    ui->hintLabel->setText(QStringLiteral("已向 %1 发送好友请求，等待对方同意…").arg(target));
}
