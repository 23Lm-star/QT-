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
    void flushfile();
    void handleCaptcha();
    void handlMoveFile();
    void handlUserInformation();
    void handlLogoff();
    void handleQueryFilePublishLink(PDU* pdu);
    void handleCreatePublishLink(PDU* pdu);
    void handleResolvePublishLink(PDU* pdu);
    // REQ-EXT-TRANSFER-001：30/32/41/42/43/102/106/108 已改由 TransferManager 按 transfer_id 路由，
    // 本类原有的单槽上传/下载接收状态已删除。
};

#endif // MSGHANDLER_H
