#include "groupfile.h"

#include "client.h"
#include "uploader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>
#include <cstring>

GroupFile *GroupFile::s_uploading = nullptr;

namespace {
QString humanSize(quint64 bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return u == 0 ? QStringLiteral("%1 B").arg(bytes)
                  : QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(units[u]));
}

QString humanTime(quint64 ms)
{
    if (ms == 0) return QStringLiteral("-");
    return QDateTime::fromMSecsSinceEpoch(qint64(ms)).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}
}

GroupFile::GroupFile(quint64 groupId, const QString &groupName, QWidget *parent)
    : QWidget(parent),
      m_groupId(groupId),
      m_groupName(groupName),
      m_fileList(new QListWidget(this)),
      m_refreshButton(new QPushButton(QStringLiteral("刷新"), this)),
      m_uploadButton(new QPushButton(QStringLiteral("上传"), this)),
      m_downloadButton(new QPushButton(QStringLiteral("下载"), this)),
      m_deleteButton(new QPushButton(QStringLiteral("删除"), this)),
      m_statusLabel(new QLabel(this))
{
    setWindowTitle(QStringLiteral("群文件 - %1").arg(groupName));
    if (parent == nullptr) resize(1240, 840);   // REQ-UX-001 A5：面积 4 倍

    auto *layout = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_uploadButton);
    toolbar->addWidget(m_downloadButton);
    toolbar->addWidget(m_deleteButton);
    layout->addLayout(toolbar);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_fileList);

    connect(m_refreshButton, &QPushButton::clicked, this, &GroupFile::refresh);
    connect(m_uploadButton, &QPushButton::clicked, this, &GroupFile::uploadFile);
    connect(m_downloadButton, &QPushButton::clicked, this, &GroupFile::downloadSelected);
    connect(m_deleteButton, &QPushButton::clicked, this, &GroupFile::deleteSelected);
    connect(m_fileList, &QListWidget::itemDoubleClicked, this, &GroupFile::onItemDoubleClicked);
    setStatus(QStringLiteral("双击文件可直接下载"));
}

GroupFile *GroupFile::uploadingInstance()
{
    return s_uploading;
}

QByteArray GroupFile::envelope(const QByteArray &body)
{
    QByteArray payload;
    groupAppendU16(payload, GROUP_PROTOCOL_VERSION);
    groupAppendU16(payload, GROUP_ENVELOPE_LENGTH);
    groupAppendU64(payload, ++m_requestId);
    groupAppendU64(payload, m_groupId);
    payload.append(body);
    return payload;
}

void GroupFile::sendRequest(uint type, const QByteArray &body)
{
    const QByteArray payload = envelope(body);
    PDU *pdu = mkPDU(uint(payload.size()));
    pdu->uiType = type;
    memcpy(pdu->caMsg, payload.constData(), size_t(payload.size()));
    Client::getinstance().seedMsg(pdu);
}

void GroupFile::setStatus(const QString &text)
{
    m_statusLabel->setText(text);
}

quint64 GroupFile::selectedGroupFileId() const
{
    const QListWidgetItem *item = m_fileList->currentItem();
    return item == nullptr ? 0 : item->data(Qt::UserRole).toULongLong();
}

void GroupFile::refresh()
{
    QByteArray body;
    groupAppendU32(body, 0);                            // offset
    groupAppendU16(body, quint16(GROUP_FILE_MAX_LIST_LIMIT));  // limit
    sendRequest(ENUM_MSG_TYPE_GROUP_FILE_LIST_REQUEST, body);
}

void GroupFile::uploadFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要上传的文件"));
    if (path.isEmpty()) return;
    const QString name = QFileInfo(path).fileName();
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("文件名不合法"));
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("无法读取所选文件"));
        return;
    }
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(1024 * 1024);
        if (chunk.isEmpty() && f.error() != QFile::NoError) {
            QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("读取文件失败"));
            return;
        }
        hasher.addData(chunk);
    }
    const qint64 size = f.size();
    f.close();
    const QByteArray shaHex = hasher.result().toHex();
    m_uploadPath = path;

    QByteArray body;
    groupAppendU64(body, quint64(size));
    groupAppendString(body, shaHex);
    groupAppendString(body, name.toUtf8());
    setStatus(QStringLiteral("上传预检查：%1").arg(name));
    sendRequest(ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST, body);
}

void GroupFile::downloadSelected()
{
    const quint64 groupFileId = selectedGroupFileId();
    if (groupFileId == 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请选择要下载的文件"));
        return;
    }
    m_downloadingRow = m_fileList->currentRow();
    QByteArray body;
    groupAppendU64(body, groupFileId);
    sendRequest(ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST, body);
}

void GroupFile::deleteSelected()
{
    const quint64 groupFileId = selectedGroupFileId();
    const QListWidgetItem *item = m_fileList->currentItem();
    if (groupFileId == 0 || item == nullptr) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请选择要删除的文件"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("删除群文件"),
                              QStringLiteral("确定从本群目录删除「%1」？").arg(item->text()))
            != QMessageBox::Yes) {
        return;
    }
    QByteArray body;
    groupAppendU64(body, groupFileId);
    sendRequest(ENUM_MSG_TYPE_GROUP_FILE_DELETE_REQUEST, body);
}

void GroupFile::onItemDoubleClicked(QListWidgetItem *)
{
    downloadSelected();
}

void GroupFile::handleResponse(PDU *pdu)
{
    if (pdu == nullptr || pdu->uiMsgLen < GROUP_ENVELOPE_LENGTH + 4) return;
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    quint32 code = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || !groupReadU32(payload, offset, code)
            || version != GROUP_PROTOCOL_VERSION || length != GROUP_ENVELOPE_LENGTH) return;
    Q_UNUSED(requestId);
    if (groupId != m_groupId) return;

    if (code != 0) {
        setStatus(QStringLiteral("群文件操作失败：%1").arg(qint32(code)));
        return;
    }

    if (pdu->uiType == ENUM_MSG_TYPE_GROUP_FILE_LIST_RESPOND) {
        quint32 count = 0, total = 0;
        if (!groupReadU32(payload, offset, count) || !groupReadU32(payload, offset, total)) return;
        m_fileList->clear();
        for (quint32 i = 0; i < count; ++i) {
            quint64 groupFileId = 0, sizeBytes = 0, createdAtMs = 0;
            QByteArray uploader, name;
            if (!groupReadU64(payload, offset, groupFileId)
                    || !groupReadU64(payload, offset, sizeBytes)
                    || !groupReadU64(payload, offset, createdAtMs)
                    || !groupReadString(payload, offset, uploader)
                    || !groupReadString(payload, offset, name)) return;
            const QString display = QString::fromUtf8(name);
            auto *item = new QListWidgetItem(QStringLiteral("%1    %2    %3    %4")
                                             .arg(display)
                                             .arg(humanSize(sizeBytes))
                                             .arg(QString::fromUtf8(uploader))
                                             .arg(humanTime(createdAtMs)), m_fileList);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(groupFileId));
            item->setData(Qt::UserRole + 1, display);
        }
        setStatus(QStringLiteral("群文件：共 %1 项（本页 %2）").arg(total).arg(count));
    } else if (pdu->uiType == ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_RESPOND) {
        if (payload.size() < GROUP_FILE_INIT_RESP_NAME_OFFSET) return;
        auto readU64At = [&](int at) -> quint64 {
            quint64 v = 0;
            for (int i = 0; i < 8; ++i) v |= quint64(quint8(payload.at(at + i))) << (8 * i);
            return v;
        };
        auto readU32At = [&](int at) -> quint32 {
            quint32 v = 0;
            for (int i = 0; i < 4; ++i) v |= quint32(quint8(payload.at(at + i))) << (8 * i);
            return v;
        };
        const bool hit = (quint8(payload.at(GROUP_FILE_INIT_RESP_HIT_OFFSET)) != 0);
        const qint64 nextOffset = qint64(readU64At(GROUP_FILE_INIT_RESP_NEXT_OFFSET));
        const quint32 nameLen = readU32At(GROUP_FILE_INIT_RESP_NAME_LEN_OFFSET);
        const QByteArray name = payload.mid(GROUP_FILE_INIT_RESP_NAME_OFFSET, int(nameLen));

        if (hit) {
            setStatus(QStringLiteral("秒传成功：%1").arg(QString::fromUtf8(name)));
            refresh();
            return;
        }
        // 未命中：走既有分块上传（31/32），续传起点由服务端给定
        if (m_uploader != nullptr) {
            setStatus(QStringLiteral("已有上传进行中"));
            return;
        }
        s_uploading = this;
        m_uploader = new Uploader(m_uploadPath, nextOffset, this);
        connect(m_uploader, &Uploader::seedMsg, &Client::getinstance(), &Client::seedMsg);
        connect(m_uploader, &Uploader::errnoMsg, this, [this](const QString &msg) {
            setStatus(QStringLiteral("上传失败：%1").arg(msg));
        });
        connect(m_uploader, &Uploader::finished, this, &GroupFile::onUploadFinished);
        setStatus(QStringLiteral("上传中：%1").arg(QString::fromUtf8(name)));
        m_uploader->start();
    } else if (pdu->uiType == ENUM_MSG_TYPE_GROUP_FILE_DELETE_RESPOND) {
        setStatus(QStringLiteral("已删除，正在刷新"));
        refresh();
    }
    // 106 下载元数据由客户端 MsgHandler 统一处理（落盘准备），此处不重复处理
}

void GroupFile::acknowledgeUploadChunk(PDU *pdu)
{
    if (m_uploader == nullptr || pdu == nullptr) return;
    bool ret = true;
    qint64 nextOffset = 0;
    bool complete = false;
    uint resumeMagic = 0;
    memcpy(&resumeMagic, pdu->caData + UPLOAD_RESUME_MAGIC_OFFSET, sizeof(resumeMagic));
    if (resumeMagic == UPLOAD_RESUME_MAGIC) {
        memcpy(&ret, pdu->caData, sizeof(ret));
        memcpy(&nextOffset, pdu->caData + UPLOAD_ACK_NEXT_OFFSET, sizeof(nextOffset));
        memcpy(&complete, pdu->caData + UPLOAD_ACK_COMPLETE_OFFSET, sizeof(complete));
    }
    m_uploader->acknowledge(ret, nextOffset, complete);
}

void GroupFile::onUploadFinished(bool success)
{
    if (s_uploading == this) s_uploading = nullptr;
    if (m_uploader != nullptr) {
        m_uploader->deleteLater();
        m_uploader = nullptr;
    }
    if (success) {
        setStatus(QStringLiteral("上传完成"));
        refresh();
    } else {
        setStatus(QStringLiteral("上传失败"));
    }
}
