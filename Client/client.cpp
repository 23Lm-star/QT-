#include "client.h"
#include "ui_client.h"
#include "process.h"
#include "index.h"
#include <string.h>
#include <QFile>
#include <QString>
#include <QDebug>
#include <QMessageBox>
#include <QHostAddress>
#include <string>
Client::Client(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Client)
{
    ui->setupUi(this);
    this->m_mhptr=new MsgHandler;
    //加载配置文件
    loadconfig();
    //生成随机验证码
    qsrand(QTime::currentTime().msec());
    ui->label_randcode->setText(generateCaptcha());
    //连接服务器
    sorket.connectToHost(QHostAddress(this->m_strlp),this->m_usPort);
    //关联连接信号和槽函数的
    //1、监听服务器的连接信号
    connect(&sorket,&QTcpSocket::connected,this,&Client::connectsort);
    //2、监听读取信号
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
    case ENUM_MSG_TYPE_SHAREFILE_FINSH_RESPOND:{//分享文件同意回应
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
    default:
        break;
    }
}
//发送消息
void Client::seedMsg(PDU *pdu){
    //利用sorket对应来发送PDU协议
    sorket.write((char*)pdu,pdu->uiTotalLen);
    //打印发送的信息
    qDebug()<<"已发送成功:"<<endl
            <<"pdu->uiTotalLen"<<pdu->uiTotalLen<<" "
            <<"pdu->uiMsgLen"<<pdu->uiMsgLen<<" "
            <<"pdu->uiType"<<pdu->uiType<<" "
            <<"pdu->caData"<<pdu->caData<<" "
            <<"pdu->caData+32"<<pdu->caData+32<<" "
            <<"pdu->caMsg"<<pdu->caMsg<<endl;
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
                    <<"pdu->caData"<<pdu->caData<<endl
                    <<"pdu->caData+32"<<pdu->caData+32<<endl
                    <<"pdu->caMsg"<<pdu->caMsg<<endl;
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
    if(strname.size()<0||strpwd.size()<0||strname.size()>32||strpwd.size()>32){
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
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_REGIST_REQUEST;
    memcpy(pdu->caData,strname.toStdString().c_str(),sizeof(strname.toStdString()));
    memcpy(pdu->caData+32,strpwd.toStdString().c_str(),sizeof(strpwd.toStdString()));
    seedMsg(pdu);
}

//发送登录信息的槽函数
void Client::on_login_PB_clicked(){
    if(strcmp(ui->label_randcode->text().toStdString().c_str(),ui->randcode_LE->text().toStdString().c_str())!=0){
        QMessageBox::information(this,"提示","验证码不正确");
        return;
    }
    //从输入框获取用户名和密码 并且 发送PDU给服务器
    QString strname=ui->name_LE->text();
    QString strpwd=ui->pwd_LE->text();
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_LOGIN_REQUEST;
    memcpy(pdu->caData,strname.toStdString().c_str(),32);
    memcpy(pdu->caData+32,strpwd.toStdString().c_str(),32);
    seedMsg(pdu);
}
//生成随机码，并填充
void Client::on_pushButton_clicked(){
    ui->label_randcode->setText(generateCaptcha());
}
