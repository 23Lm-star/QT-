#ifndef MSGHANDLER_H
#define MSGHANDLER_H

#include "protocol.h"

#include <QString>
#include <QFile>

class MsgHandler
{
public:
    PDU* pdu;
    MsgHandler();
    void handlrigist();
    void handllogin();
    void handlfinduser();
    void handlonlineuser();
    void handlAddFriend();
    void handlAddFriendAgree(const QString& myName);
    void handlFlushFriend();
    void handlDeleteFriend();
    void chat();
    void getcharmsg();
    void mkdir();
    void deldir();
    void changedir();
    void renamedir();
    void sharefileagree();
    void sharefilefinsh();
    void uploadfileinit();
    void uploadfiledata();
    void flushfile();
    void downloadfile();
    void handleCaptcha();
    void handlMoveFile();
    void handlUserInformation();

    //流式下载接收状态
    void handleDownloadRespond();
    void handleDownloadData();
    void handleDownloadFinish();

private:
    QFile* m_recvFile;        //下载接收文件
    QString m_recvFilePath;   //保存路径
    QString m_recvFileName;   //文件名
};

#endif // MSGHANDLER_H
