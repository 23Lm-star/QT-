#ifndef MSGHANDLER_H
#define MSGHANDLER_H

#include "protocol.h"

#include <QString>

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
};

#endif // MSGHANDLER_H
