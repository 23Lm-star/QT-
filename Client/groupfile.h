#ifndef GROUPFILE_H
#define GROUPFILE_H

#include "protocol.h"
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

class Uploader;

// REQ-EXT-GROUP-003：单群文件目录窗口。
// 目录经 99/100 分页列出（名称/大小/上传者/时间，无物理路径）；
// 上传先发 101 做秒传预检查：命中零正文，未命中回 session_id/nextOffset 走既有 31 分块；
// 下载发 105 取 106 元数据后走既有 42/43 分块；删除发 103。
// 权限一律由服务端判定（本窗口只做展示与置灰，不缓存授权）。
class GroupFile : public QWidget
{
    Q_OBJECT
public:
    GroupFile(quint64 groupId, const QString &groupName, QWidget *parent = nullptr);
    quint64 groupId() const { return m_groupId; }
    QString groupName() const { return m_groupName; }

    // 100/102/104 响应路由（按信封 group_id 匹配）
    void handleResponse(PDU *pdu);
    // 32 上传数据响应路由（仅当本窗口有进行中的群上传）
    void acknowledgeUploadChunk(PDU *pdu);
    // 当前有进行中群上传的窗口（无则 nullptr）；供 Client 分发 32 响应
    static GroupFile *uploadingInstance();
    // 42/43 下载流前置：106 元数据已由客户端 MsgHandler 处理（复用既有接收文件）

private slots:
    void refresh();
    void uploadFile();
    void downloadSelected();
    void deleteSelected();
    void onUploadFinished(bool success);
    void onItemDoubleClicked(QListWidgetItem *item);

private:
    void sendRequest(uint type, const QByteArray &body = QByteArray());
    QByteArray envelope(const QByteArray &body);
    void setStatus(const QString &text);
    quint64 selectedGroupFileId() const;

    quint64 m_groupId;
    QString m_groupName;
    quint64 m_requestId = 0;
    QListWidget *m_fileList;
    QPushButton *m_refreshButton;
    QPushButton *m_uploadButton;
    QPushButton *m_downloadButton;
    QPushButton *m_deleteButton;
    QLabel *m_statusLabel;
    int m_downloadingRow = -1;   // 正在下载的行（用于完成提示与去重）

    // 群上传（秒传未命中时的分块路径）
    Uploader *m_uploader = nullptr;
    QString m_uploadPath;
    static GroupFile *s_uploading;
};

#endif // GROUPFILE_H
