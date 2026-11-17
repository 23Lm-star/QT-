#ifndef MSGHANDLER_H
#define MSGHANDLER_H
// 客户端业务响应处理器：由 Client::handlMsg 按消息类型调用。
// 约定：每次处理前 Client 把当前 PDU 指针写入 pdu 成员，处理函数直接读 caData/caMsg；
// 本类不做线程调度，全部运行在 GUI 线程（可安全弹窗、操作界面）。
// 多数处理函数的 caData 首 4 字节是 ret 码，其余字段从固定偏移读取。

#include "protocol.h"
#include <QString>
#include <QFile>

class MsgHandler
{
public:
    // 当前待处理的 PDU；指向 Client 接收缓冲内部，仅在本轮分发期间有效，不拥有所有权。
    PDU* pdu;
    MsgHandler();
    // 处理注册响应：ret>0 成功（延时提示并建用户目录），-1 失败，-2/-3 验证码错误/过期，-4 锁定。
    void handlrigist();
    // REQ-EXT-TRANSFER-003 R-1：登录成功时**同步**返回登录名（失败/异常返回空串）。
    // 调用方必须把它写进 Client::m_LoginName 之后再做任何依赖用户名的动作——
    // 1.5 s 后的 UI 定时器不再承担名字写入。
    QString handllogin();
    // 处理查找用户结果：1 在线、0 不在线、2 用户不存在。
    void handlfinduser();
    // 处理在线用户列表：caMsg 为若干个 32 字节定长用户名，排除自己后刷新列表。
    void handlonlineuser();
    // 处理加好友响应：1 成功、-2 对方不在线、其它为通用失败。
    void handlAddFriend();
    // 对方加好友请求：弹窗确认后以 myName 回复 ADDFRIEND_RESPOND（caData 前 32=我，后 32=对方）。
    void handlAddFriendAgree(const QString& myName);
    // 处理好友列表：caMsg 为 32 字节定长用户名数组，刷新好友界面。
    void handlFlushFriend();
    // 处理删好友响应，成功则刷新好友列表。
    void handlDeleteFriend();
    // 收到聊天消息：caMsg 内嵌 Msg（totalLen/msgLen/Time/msg[]），显示并弹出聊天窗。
    void chat();
    // 处理离线消息批量响应：caMsg 连续排布多个 Msg，按 totalLen 步进解析。
    void getcharmsg();
    // REQ-CLI-MSGSTORE-001：服务端 16 私聊入库应答（只决定自发消息是否落本地库，不改界面语义）
    void handleChatAck();
    // ===== REQ-CHAT-ACK-001：桌面端私聊接收确认 =====
    // 109 实时投递：caData[0,32)=发送人、[32,40)=消息 ID、caMsg=Msg 记录
    void chatDeliver();
    // 112 待送达批量响应：每条 qint64 id + qint32 正文长 + qint32 时间长 + 正文 + 时间
    void getPendingChat();
    // 114 待处理好友申请：caMsg 为 32 字节定长请求方账号数组，逐条弹窗询问
    void handlePendingFriendList();
    // REQ-PAGE-FETCH-001：118 聊天历史分页响应，解析后交给 Chat 窗口渲染与落库
    void chatHistoryResp();
    // 把已处理的消息 ID 批量回 110（服务端据此置已送达）
    void sendChatDeliveredAck(const QList<qint64> &ids);
    // 处理建目录响应，成功则刷新文件列表。
    void mkdir();
    // 处理删除响应：成功则重新请求当前目录列表（异常时名称为“文件夹”）。
    void deldir();
    // 处理切换目录响应：成功用 caMsg 中的新路径刷新，失败恢复路径输入框。
    void changedir();
    // 处理重命名响应，成功则请求刷新当前目录。
    void renamedir();
    // 对方发来的分享文件请求：确认后回送 AGRE 响应，原样带回路径。
    void sharefileagree();
    // 分享接收完成响应：成功则刷新文件列表。
    void sharefilefinsh();
    // 处理文件列表：caMsg 为 FileInfo 数组，转换为堆分配的列表交给文件页刷新。
    void flushfile();
    // 处理验证码响应：status=0 成功下发验证码；-5 冷却中；-6 已锁定（10 分钟）。
    void handleCaptcha();
    // 处理移动文件响应，成功则刷新文件列表。
    void handlMoveFile();
    // 处理注销账号响应：成功则回登录页并断开连接。
    void handlLogoff();
    // 处理“查询文件已有分享链接”：有效则展示链接，过期/撤销则进入恢复流程。
    void handleQueryFilePublishLink(PDU* pdu);
    // 处理“创建/续期分享链接”：成功提示链接，失效状态转入恢复选择框。
    void handleCreatePublishLink(PDU* pdu);
    // 处理“解析分享链接”：成功提示服务端保存路径并刷新文件列表。
    void handleResolvePublishLink(PDU* pdu);
    // REQ-EXT-TRANSFER-001：30/32/41/42/43/102/106/108 已改由 TransferManager 按 transfer_id 路由，
    // 本类原有的单槽上传/下载接收状态已删除。
};

#endif // MSGHANDLER_H
