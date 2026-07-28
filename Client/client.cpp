#include "client.h"
#include "ui_client.h"
#include "process.h"
#include "index.h"
#include <string.h>
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QDebug>
#include <QMessageBox>
#include <QHostAddress>
#include <QCryptographicHash>
#include <QGraphicsOpacityEffect>
#include <QTimer>
#include <QInputDialog>
#include <string>

namespace {
QByteArray passwordHashForProtocol(const QString &password)
{
    return QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex();
}

void copyFixedField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize - 1));
}

void copyExactField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize));
}
}

Client::Client(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Client)
{
    ui->setupUi(this);
    setFixedSize(600,420);
    this->m_mhptr=new MsgHandler;
    //加载配置文件
    loadconfig();
    //隐藏加载动画层
    ui->loadingLabel->hide();
    //验证码冷却定时器
    m_captchaCooldownSec=0;
    m_captchaCooldownTimer=new QTimer(this);
    connect(m_captchaCooldownTimer,&QTimer::timeout,this,&Client::onCaptchaCooldownTick);
    //连接服务器
    sorket.connectToHost(QHostAddress(this->m_strlp),this->m_usPort);
    //关联连接信号和槽函数的
    connect(&sorket,&QTcpSocket::connected,this,&Client::connectsort);
    connect(&sorket,&QTcpSocket::readyRead,this,&Client::recvmsg);
}

Client::~Client(){
    delete ui;
}

//加载配置文件
void Client::loadconfig()
{
    QFile f(":/connect.config");//配置文件用: + 前缀 + 文件名
    if(f.open(QIODevice::ReadOnly)){//打开文件成功
        QString s=QString(f.readAll());//读取打开文件中的内容并且转换成QString
        //将配置文件中的ip地址和端口号取出来并且存到变量里面
        QStringList str=s.split("\r\n");
        m_strlp=str[0];
        m_usPort=str[1].toUShort();
        m_strRootPath=str[2];
        qDebug()<<"loadconfig ip地址"<<m_strlp<<"端口号:"<<m_usPort<<"文件路径"<<m_strRootPath<<endl;
        f.close();//关闭配置文件
    }
    else {//打开文件失败
        //开启提示窗口 打印错误信息
        QMessageBox::critical(this,"提示","打开文件失败");
        qDebug() << "loadconfig 打开文件失败"<<endl;
    }
}

//构建唯一示例，获取唯一实例的函数
Client &Client::getinstance(){
    //静态实例对象来保证唯一实例
    static Client instance;
    return instance;
}
//刷新用户名输入框和密码输入框
void Client::flus()
{
    ui->name_LE->clear();
    ui->pwd_LE->clear();
}

QString Client::generateCaptcha(int length)
{
   const QString chars = "RSTnoprsXYZabcdeJK0QtuvwxyzUVWq123456789LMNPfghijklABCDEFGHm";  // 去除易混淆字符,随机码获取串
   //存储随机验证码
   QString captcha;
   for (int i = 0; i < length; ++i) {//依次随机获取字符
       int index = qrand() % chars.length();//生成[0, length)的随机数
       captcha.append(chars.at(index));
   }
   return captcha;
}


//进行接受消息的处理
void Client::handlMsg(PDU *&pdu)
{
//    if(m_mhptr!=nullptr){
//        delete m_mhptr;
//    }
    m_mhptr->pdu=pdu;
    switch(pdu->uiType){
    case ENUM_MSG_TYPE_REGIST_RESPOND:{//注册用户
        m_mhptr->handlrigist();
        break;
    }
    case ENUM_MSG_TYPE_LOGIN_RESPOND:{//登录用户
        m_mhptr->handllogin();
        break;
    }
    case ENUM_MSG_TYPE_findUser_RESPOND:{//寻找用户
        m_mhptr->handlfinduser();
        break;
    }
    case ENUM_MSG_TYPE_ONLINEUSER_RESPOND:{//获取所有在线用户
        m_mhptr->handlonlineuser();
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_RESPOND:{//添加好友回应（添加好友回应）
        m_mhptr->handlAddFriend();
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_REQUEST:{//添加好友请求（请求添加好友）
        m_mhptr->handlAddFriendAgree(m_LoginName);
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND:{//刷新好友回应
        m_mhptr->handlFlushFriend();
        break;
    }
    case ENUM_MSG_TYPE_DELETEFRIEND_RESPOND:{//删除好友
        m_mhptr->handlDeleteFriend();
        break;
    }
    case ENUM_MSG_TYPE_CHAT_REQUEST:{//即时聊天
        m_mhptr->chat();
        break;
    }
    case ENUM_MSG_TYPE_GETCHATMSG_RESPOND:{//获取所有没有收到的聊天消息
        m_mhptr->getcharmsg();
        break;
    }
    case ENUM_MSG_TYPE_MKDIR_RESPOND:{//创建目录
        m_mhptr->mkdir();
        break;
    }
    case ENUM_MSG_TYPE_DELFILE_RESPOND:{//删除目录
        m_mhptr->deldir();
        break;
    }
    case ENUM_MSG_TYPE_CHANGEDIR_RESPOND:{//删除目录
        m_mhptr->changedir();
        break;
    }
    case ENUM_MSG_TYPE_RENAMEFILE_RESPOND:{//重命名文件/目录
        m_mhptr->renamedir();
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFILE_RESPOND:{//刷新文件
        m_mhptr->flushfile();
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_RESPOND:{//分享文件回应
        QMessageBox::information(&Index::getinstance(),"提示","文件分享完成");
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_AGREE_REQUEST:{//分享文件同意回应
        m_mhptr->sharefileagree();
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND:{//分享文件同意回应
        m_mhptr->sharefilefinsh();
        break;
    }
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND:{//删除目录
        m_mhptr->uploadfiledata();
        break;
    }
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND:{//重命名文件/目录
        m_mhptr->uploadfileinit();
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND:{
        m_mhptr->downloadfile();
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_DATA:{
        m_mhptr->handleDownloadData();
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_FINISH:{
        m_mhptr->handleDownloadFinish();
        break;
    }
    case ENUM_MSG_TYPE_CAPTCHA_RESPOND:{
        m_mhptr->handleCaptcha();
        break;
    }
    case ENUM_MSG_TYPE_MOVEFILE_RESPOND:{
        m_mhptr->handlMoveFile();
        break;
    }
    case ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND:{
        m_mhptr->handlUserInformation();
        break;
    }
    case ENUM_MSG_TYPE_LOGOFF_RESPOND:{
        m_mhptr->handlLogoff();
        break;
    }
    default:
        break;
    }
}
//发送消息
// 发送 PDU 到服务端。pdu 必须由 mkPDU() (malloc) 分配。
// 发送完成后调用 free(pdu) 释放。
// 调用方不得在 seedMsg 返回后再访问 pdu。
void Client::seedMsg(PDU *pdu){
    //利用sorket对应来发送PDU协议
    sorket.write((char*)pdu,pdu->uiTotalLen);
    //打印发送的信息
    qDebug()<<"已发送成功:"<<endl
            <<"pdu->uiTotalLen"<<pdu->uiTotalLen<<" "
            <<"pdu->uiMsgLen"<<pdu->uiMsgLen<<" "
            <<"pdu->uiType"<<pdu->uiType<<" "
            <<"pdu->caData"<<QByteArray(pdu->caData,32)<<" "
            <<"pdu->caData+32"<<QByteArray(pdu->caData+32,32)<<" "
            <<"pdu->caMsg"<<QByteArray(pdu->caMsg,pdu->uiMsgLen)<<endl;
    //清理资源
    free(pdu);
    pdu=nullptr;
}
//成功连接服务器的信息反馈函数
void Client::connectsort(){
    qDebug()<<"服务器连接成功"<<endl;
}

//用于接受服务器消息
void Client::recvmsg(){
    //现将所有的消息按照字节数组的形式存入m_pdulist
    m_pdulist.append(sorket.readAll());
    //然后将m_pdulist里面的 发送完整的PDU 依次取出来然后进行处理
    while (m_pdulist.size()>=(int)sizeof(PDU)) {//若是里面可能含有一个完整的PDU
        PDU*pdu=(PDU*)m_pdulist.data();//利用一个PDU指针指向待取的PDU对象
        //先读取 pdu 总长度这个成员变量
        uint pdutotallen=pdu->uiTotalLen;//找出待取的PDU对象的总长度
        if((uint)m_pdulist.size()>=pdutotallen){//若是待取的PDU对象是完整的，就进行处理
            //打印接受PDU的信息
            qDebug()<<"服务器响应 已接收成功:"<<endl
                    <<"pdu->uiTotalLen"<<pdu->uiTotalLen<<endl
                    <<"pdu->uiMsgLen"<<pdu->uiMsgLen<<endl
                    <<"pdu->uiType"<<pdu->uiType<<endl
                    <<"pdu->caData"<<QByteArray(pdu->caData,32)<<endl
                    <<"pdu->caData+32"<<QByteArray(pdu->caData+32,32)<<endl
                    <<(pdu->uiType==ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND || pdu->uiType==ENUM_MSG_TYPE_DOWNLOADFILE_DATA ? "pdu->caMsg bytes" : "pdu->caMsg")
                    <<(pdu->uiType==ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND || pdu->uiType==ENUM_MSG_TYPE_DOWNLOADFILE_DATA ? QByteArray::number(pdu->uiMsgLen).constData() : pdu->caMsg)
                    <<endl;
            handlMsg(pdu);//消息处理 接收的pdu
            m_pdulist.remove(0,pdu->uiTotalLen);//将接受的完整PDU对象从m_pdulist中删除
        }
        else break;
    }
}

//发送 注册信息报文 的 槽函数
void Client::on_register_PB_clicked(){
    //获取用户名和密码的内容，并且检查用户名和密码是否合法
    QString strname=ui->name_LE->text();
    QString strpwd=ui->pwd_LE->text();
    if(strname.isEmpty()||strpwd.isEmpty()||strname.toUtf8().size()>31){
        QMessageBox::information(this,"提示","用户名或密码长度非法");
        return ;
    }
    else {//检查 用户名是否是由数字和大小写字母组成
        for(int i=0;i<strname.size();i++){
            if(!((strname.toStdString()[i]>='0' && strname.toStdString()[i]<='9')
                 ||(strname.toStdString()[i]>='a' && strname.toStdString()[i]<='z')
                 ||(strname.toStdString()[i]>='A' && strname.toStdString()[i]<='Z') ) ){
                QMessageBox::information(this,"提示","用户名只能由数字和大小写字母组成");
                return ;
            }
        }
    }
    //构建注册信息的PDU协议，并且给其 类型，用户名，密码 进行赋值
    QByteArray captchaBytes=ui->randcode_LE->text().toUtf8();
    PDU* pdu=mkPDU(captchaBytes.size()+1);
    pdu->uiType=ENUM_MSG_TYPE_REGIST_REQUEST;
    QByteArray passwordHash=passwordHashForProtocol(strpwd);
    copyFixedField(pdu->caData,32,strname.toUtf8());
    copyExactField(pdu->caData+32,32,passwordHash);
    memcpy(pdu->caMsg,captchaBytes.constData(),captchaBytes.size()+1);
    qDebug()<<"register encrypted password"<<passwordHash;
    seedMsg(pdu);
}

//发送登录信息的槽函数
void Client::on_login_PB_clicked(){
    QString captchaInput=ui->randcode_LE->text();
    if(captchaInput.isEmpty()){
        QMessageBox::information(this,"提示","请输入验证码");
        return;
    }
    //从输入框获取用户名和密码 并且 发送PDU给服务器
    QString strname=ui->name_LE->text();
    QString strpwd=ui->pwd_LE->text();
    if(strname.isEmpty()||strpwd.isEmpty()||strname.toUtf8().size()>31){
        QMessageBox::information(this,"提示","用户名或密码长度非法");
        return;
    }
    QByteArray captchaBytes=captchaInput.toUtf8();
    PDU* pdu=mkPDU(captchaBytes.size()+1);
    pdu->uiType=ENUM_MSG_TYPE_LOGIN_REQUEST;
    QByteArray passwordHash=passwordHashForProtocol(strpwd);
    copyFixedField(pdu->caData,32,strname.toUtf8());
    copyExactField(pdu->caData+32,32,passwordHash);
    memcpy(pdu->caMsg,captchaBytes.constData(),captchaBytes.size()+1);
    qDebug()<<"[DIAG-CLIENT] login: name="<<strname<<" raw_pwd_len="<<strpwd.size()<<" hash_hex="<<passwordHash<<" hash_len="<<passwordHash.size();
    qDebug()<<"[DIAG-CLIENT] caData+32 raw bytes:"<<QByteArray(pdu->caData+32,32).toHex();
    seedMsg(pdu);
}
void Client::updateCaptchaDisplay(const QString& code){
    QMessageBox mb(this);
    mb.setWindowTitle("验证码");
    mb.setText(QString("您的验证码为：\n\n    %1    \n\n请在1分钟内输入").arg(code));
    mb.setIcon(QMessageBox::Information);
    mb.exec();
}
void Client::resetCaptchaButton(){
    ui->pushButton->setEnabled(true);
    ui->pushButton->setText("获取验证码");
}
void Client::startCaptchaCooldown(){
    m_captchaCooldownSec=60;
    ui->pushButton->setEnabled(false);
    ui->pushButton->setText(QString("请等待 %1 秒").arg(m_captchaCooldownSec));
    m_captchaCooldownTimer->start(1000);
}
void Client::onCaptchaCooldownTick(){
    m_captchaCooldownSec--;
    if(m_captchaCooldownSec<=0){
        m_captchaCooldownTimer->stop();
        ui->pushButton->setEnabled(true);
        ui->pushButton->setText("获取验证码");
        return;
    }
    ui->pushButton->setText(QString("请等待 %1 秒").arg(m_captchaCooldownSec));
}
//请求服务端验证码
void Client::on_pushButton_clicked(){
    if(sorket.state()!=QAbstractSocket::ConnectedState){
        QMessageBox::information(this,"提示","未连接服务器，无法获取验证码");
        return;
    }
    if(m_captchaCooldownSec>0){
        QMessageBox::information(this,"提示",QString("验证码尚未过期，请等待 %1 秒后再获取").arg(m_captchaCooldownSec));
        return;
    }
    ui->pushButton->setEnabled(false);
    ui->pushButton->setText("获取中...");
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_CAPTCHA_REQUEST;
    seedMsg(pdu);
}
//登录/注册成功动画
void Client::showLoadingAnimation(bool success){
    ui->loadingLabel->raise();
    ui->loadingLabel->setText(success ? "✓ 验证成功\n正在进入..." : "✗ 操作失败");
    ui->loadingLabel->setStyleSheet(success
        ? "background:rgba(11,61,94,220);color:#2ecc71;font-size:22px;font-weight:bold;"
        : "background:rgba(11,61,94,220);color:#e74c3c;font-size:22px;font-weight:bold;");
    QGraphicsOpacityEffect* effect=new QGraphicsOpacityEffect(this);
    ui->loadingLabel->setGraphicsEffect(effect);
    ui->loadingLabel->show();
    QPropertyAnimation* anim=new QPropertyAnimation(effect,"opacity");
    anim->setDuration(800);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim,&QPropertyAnimation::finished,this,[this](){
        QTimer::singleShot(600,this,[this](){
            auto* eff = qobject_cast<QGraphicsOpacityEffect*>(
                ui->loadingLabel->graphicsEffect());
            if (!eff) return;
            QPropertyAnimation* fadeOut=new QPropertyAnimation(eff,"opacity");
            fadeOut->setDuration(400);
            fadeOut->setStartValue(1.0);
            fadeOut->setEndValue(0.0);
            fadeOut->setEasingCurve(QEasingCurve::InCubic);
            connect(fadeOut,&QPropertyAnimation::finished,this,[this](){
                ui->loadingLabel->hide();
                ui->loadingLabel->setGraphicsEffect(nullptr);
            });
            fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
        });
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}
//发送注销账户请求
void Client::sendLogoffRequest()
{
    //弹出密码确认框
    bool ok=false;
    QString pwdInput=QInputDialog::getText(this,"注销确认",
        "输入密码以确认注销账户：\n（此操作不可逆，将永久删除您的账户、好友关系和所有文件）",
        QLineEdit::Password,"",&ok);
    if(!ok||pwdInput.isEmpty()){
        return;
    }

    //二次确认
    int confirm=QMessageBox::warning(this,"最后确认",
        "确定要永久删除您的账户吗？\n此操作不可撤销！",
        QMessageBox::Yes|QMessageBox::No,QMessageBox::No);
    if(confirm!=QMessageBox::Yes){
        return;
    }

    QByteArray passwordHash=passwordHashForProtocol(pwdInput);
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_LOGOFF_REQUEST;
    copyFixedField(pdu->caData,32,m_LoginName.toUtf8());
    copyExactField(pdu->caData+32,32,passwordHash);
    seedMsg(pdu);
}
