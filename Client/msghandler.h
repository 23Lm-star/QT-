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
    void uploadfilefastcheck();//REQ-EXT-FILE-002：秒传预检查响应（命中提示+刷新，未命中续传）
    void uploadfiledata();
    void flushfile();
    void downloadfile();
    void handleCaptcha();
    void handlMoveFile();
    void handlUserInformation();
    void handlLogoff();
    void handleQueryFilePublishLink(PDU* pdu);
    void handleCreatePublishLink(PDU* pdu);
    void handleResolvePublishLink(PDU* pdu);
    void handleDownloadRespond();
    // REQ-EXT-GROUP-003：106 群文件下载元数据响应（正文复用既有 42/43 接收流）
    void handleGroupFileDownloadRespond(PDU* pdu);

    //流式下载接收状态
    void handleDownloadData();
    void handleDownloadFinish();

private:
    // 为流式下载创建落盘文件（同名 (N) 递推）；成功返回 true 并置 m_recvFile/m_recvFilePath
    bool beginReceiving(const QString& fileName);
    QFile* m_recvFile;        //下载接收文件
    QString m_recvFilePath;   //保存路径
    QString m_recvFileName;   //文件名
};

#endif // MSGHANDLER_H
