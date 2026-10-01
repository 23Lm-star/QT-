#include "transfertask.h"

#include "client.h"
#include "file.h"
#include "index.h"
#include "logging.h"
#include "uploader.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cstring>

// ===================== TransferManager =====================

TransferManager& TransferManager::instance()
{
    static TransferManager mgr;
    return mgr;
}

TransferManager::TransferManager()
{
}

quint64 TransferManager::allocId()
{
    return m_nextId++;
}

QString TransferManager::stateText(State s)
{
    switch (s) {
    case Queued:   return QStringLiteral("排队中");
    case Hashing:  return QStringLiteral("计算摘要…");
    case Running:  return QStringLiteral("传输中");
    case Paused:   return QStringLiteral("已暂停");
    case Done:     return QStringLiteral("已完成");
    case Failed:   return QStringLiteral("失败");
    case Cancelled:return QStringLiteral("已取消");
    }
    return QString();
}

// 41 的 id 落在 [16..23]，其余传输族 [48..55]
quint64 TransferManager::transferIdOf(const PDU* pdu)
{
    quint64 id = 0;
    if (pdu == nullptr) return 0;
    const int off = (pdu->uiType == ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND)
            ? DOWNLOAD_RESP_TRANSFER_ID_OFFSET : TRANSFER_ID_OFFSET;
    memcpy(&id, pdu->caData + off, sizeof(id));
    return id;
}

quint64 TransferManager::stableKeyOf(quint64 transferId)
{
    return m_idToKey.value(transferId, 0);
}

QList<quint64> TransferManager::taskKeys() const
{
    QList<quint64> keys = m_tasks.keys();
    std::sort(keys.begin(), keys.end());
    return keys;
}

quint64 TransferManager::enqueuePersonalUpload(const QString& localPath, const QString& dirPath, const QString& displayName)
{
    Task t;
    t.key = m_nextKey++;
    t.id = allocId();
    t.kind = PersonalUpload;
    t.localPath = localPath;
    t.dirPath = dirPath;
    t.displayName = displayName;
    t.totalBytes = QFileInfo(localPath).size();
    t.state = Queued;
    m_tasks.insert(t.key, t);
    m_idToKey.insert(t.id, t.key);
    m_uploadQueue.append(t.key);
    emit tasksChanged();
    pump();
    return t.key;
}

quint64 TransferManager::enqueueGroupUpload(quint64 groupId, const QString& localPath,
                                            const QString& displayName, const QByteArray& shaHex)
{
    Task t;
    t.key = m_nextKey++;
    t.id = allocId();
    t.kind = GroupUpload;
    t.localPath = localPath;
    t.groupId = groupId;
    t.displayName = displayName;
    t.shaHex = shaHex;
    t.totalBytes = QFileInfo(localPath).size();
    t.state = Queued;
    m_tasks.insert(t.key, t);
    m_idToKey.insert(t.id, t.key);
    m_uploadQueue.append(t.key);
    emit tasksChanged();
    pump();
    return t.key;
}

quint64 TransferManager::enqueuePersonalDownload(const QString& serverPath, const QString& displayName)
{
    Task t;
    t.key = m_nextKey++;
    t.id = allocId();
    t.kind = PersonalDownload;
    t.serverPath = serverPath;
    t.displayName = displayName;
    t.state = Queued;
    m_tasks.insert(t.key, t);
    m_idToKey.insert(t.id, t.key);
    m_downloadQueue.append(t.key);
    emit tasksChanged();
    pump();
    return t.key;
}

quint64 TransferManager::enqueueGroupDownload(quint64 groupId, quint64 groupFileId, const QString& displayName)
{
    Task t;
    t.key = m_nextKey++;
    t.id = allocId();
    t.kind = GroupDownload;
    t.groupId = groupId;
    t.groupFileId = groupFileId;
    t.displayName = displayName;
    t.state = Queued;
    m_tasks.insert(t.key, t);
    m_idToKey.insert(t.id, t.key);
    m_downloadQueue.append(t.key);
    emit tasksChanged();
    pump();
    return t.key;
}

// 两方向预算独立（D6）：上传 3 / 下载 3，各自覆盖个人与群路径
void TransferManager::pump()
{
    while (m_activeUploads < int(MAX_PARALLEL_UPLOADS) && !m_uploadQueue.isEmpty()) {
        const quint64 key = m_uploadQueue.takeFirst();
        Task* t = taskByKey(key);
        if (t == nullptr || t->state != Queued) continue;
        startTask(*t);
    }
    while (m_activeDownloads < int(MAX_PARALLEL_DOWNLOADS) && !m_downloadQueue.isEmpty()) {
        const quint64 key = m_downloadQueue.takeFirst();
        Task* t = taskByKey(key);
        if (t == nullptr || t->state != Queued) continue;
        startTask(*t);
    }
}

// M5：SHA-256 计算移出 UI 线程（1MiB 分块，算法与分段不变）；完成后回主线程发 101
void TransferManager::startGroupHash(Task& t)
{
    t.state = Hashing;
    ++m_activeUploads;
    const quint64 key = t.key;
    auto* watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, key, watcher]() {
        const QByteArray sha = watcher->result();
        watcher->deleteLater();
        Task* tt = taskByKey(key);
        if (tt == nullptr) return;
        if (sha.isEmpty()) {
            finishTask(*tt, Failed, QStringLiteral("读取文件失败"));
            return;
        }
        tt->shaHex = sha;
        tt->state = Running;   // 名额已在 Hashing 时计入，不重复
        sendGroupUploadInit(*tt);
        emit tasksChanged();
    });
    const QString path = t.localPath;
    watcher->setFuture(QtConcurrent::run([path]() -> QByteArray {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        QCryptographicHash hasher(QCryptographicHash::Sha256);
        while (!f.atEnd()) {
            const QByteArray chunk = f.read(1024 * 1024);
            if (chunk.isEmpty() && f.error() != QFile::NoError) return QByteArray();
            hasher.addData(chunk);
        }
        return hasher.result().toHex();
    }));
    emit tasksChanged();
}

void TransferManager::sendGroupUploadInit(Task& t)
{
    QByteArray payload;
    groupAppendU16(payload, GROUP_PROTOCOL_VERSION);
    groupAppendU16(payload, GROUP_ENVELOPE_LENGTH);
    groupAppendU64(payload, t.id);            // request_id
    groupAppendU64(payload, t.groupId);
    groupAppendU64(payload, quint64(t.totalBytes));
    groupAppendString(payload, t.shaHex);
    groupAppendString(payload, t.displayName.toUtf8());
    PDU* pdu = mkPDU(uint(payload.size()));
    pdu->uiType = ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST;
    memcpy(pdu->caData + TRANSFER_ID_OFFSET, &t.id, sizeof(t.id));
    memcpy(pdu->caMsg, payload.constData(), size_t(payload.size()));
    Client::getinstance().seedMsg(pdu);
    emit tasksChanged();
}

// 单飞：同一时刻只允许一个 97 在途（服务端预检查落在旧单流槽）
void TransferManager::pumpFastcheck()
{
    if (m_fastcheckInFlight != 0) return;
    while (!m_fastcheckQueue.isEmpty()) {
        const quint64 key = m_fastcheckQueue.takeFirst();
        Task* t = taskByKey(key);
        if (t == nullptr || t->state != Hashing) continue;
        m_fastcheckInFlight = key;
        emit fastcheckDue(key);
        return;
    }
}

void TransferManager::submitFastcheck(quint64 key, const QByteArray& shaHex)
{
    Task* t = taskByKey(key);
    if (t == nullptr) return;
    t->shaHex = shaHex;
    m_fastcheckQueue.append(key);
    emit tasksChanged();
    pumpFastcheck();
}

void TransferManager::failTask(quint64 key, const QString& reason)
{
    Task* t = taskByKey(key);
    if (t == nullptr) return;
    m_fastcheckQueue.removeOne(key);
    if (m_fastcheckInFlight == key) m_fastcheckInFlight = 0;
    finishTask(*t, Failed, reason);
}

// 97 → 98 → 29（服务端采纳 97 建立的暂存会话）→ 30 → 31/32
void TransferManager::onFastcheckRespond(PDU* pdu)
{
    const quint64 key = m_fastcheckInFlight;
    m_fastcheckInFlight = 0;
    Task* t = taskByKey(key);
    if (t == nullptr) {
        pumpFastcheck();
        return;
    }
    bool ret = false, hit = false;
    uint version = 0;
    memcpy(&ret, pdu->caData + FASTCHECK_RESP_RET_OFFSET, sizeof(ret));
    memcpy(&hit, pdu->caData + FASTCHECK_RESP_HIT_OFFSET, sizeof(hit));
    memcpy(&version, pdu->caData + FASTCHECK_RESP_VERSION_OFFSET, sizeof(version));
    if (version != FASTCHECK_VERSION) {
        finishTask(*t, Failed, QStringLiteral("服务端协议版本不匹配，请升级客户端"));
        pumpFastcheck();
        return;
    }
    if (!ret) {
        finishTask(*t, Failed, QStringLiteral("上传失败：服务端拒绝秒传预检查"));
        pumpFastcheck();
        return;
    }
    if (hit) {
        finishTask(*t, Done);   // 秒传命中：零正文，服务端已建条目
        pumpFastcheck();
        return;
    }
    sendPersonalUploadInit(*t);
    pumpFastcheck();
}

void TransferManager::sendPersonalUploadInit(Task& t)
{
    const qint64 size = QFileInfo(t.localPath).size();
    const QByteArray nameBytes = t.displayName.toUtf8();
    const QByteArray dirBytes = t.dirPath.toUtf8();
    PDU* pdu = mkPDU(uint(dirBytes.size() + 1));
    pdu->uiType = ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST;
    memcpy(pdu->caData, nameBytes.constData(), qMin(nameBytes.size(), 31));
    memcpy(pdu->caData + UPLOAD_FILE_SIZE_OFFSET, &size, sizeof(size));
    const uint magic = UPLOAD_RESUME_MAGIC;
    memcpy(pdu->caData + UPLOAD_RESUME_MAGIC_OFFSET, &magic, sizeof(magic));
    memcpy(pdu->caData + TRANSFER_ID_OFFSET, &t.id, sizeof(t.id));
    memcpy(pdu->caMsg, dirBytes.constData(), size_t(dirBytes.size()));
    t.totalBytes = size;
    t.state = Running;
    Client::getinstance().seedMsg(pdu);
    emit tasksChanged();
}

void TransferManager::startTask(Task& t)
{
    if (t.kind == GroupUpload && t.shaHex.isEmpty()) {
        startGroupHash(t);
        return;
    }
    if (t.kind == PersonalUpload) {
        ++m_activeUploads;
        if (t.needFastcheck) {
            t.state = Hashing;              // SHA 由 File 在后台计算（M5），随后单飞发 97
            emit hashDue(t.key, t.localPath);
        } else {
            sendPersonalUploadInit(t);      // 续传：直接 29（服务端由确定性 staging 回带续传起点）
        }
        emit tasksChanged();
        return;
    }
    Client& cli = Client::getinstance();
    if (t.kind == GroupUpload) {
        t.state = Running;
        ++m_activeUploads;
        sendGroupUploadInit(t);
        return;
    } else if (t.kind == PersonalDownload) {
        const QByteArray nameBytes = t.displayName.toUtf8();
        const QByteArray pathBytes = t.serverPath.toUtf8();
        PDU* pdu = mkPDU(uint(pathBytes.size() + 1));
        pdu->uiType = ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST;
        memcpy(pdu->caData, nameBytes.constData(), qMin(nameBytes.size(), 31));
        memcpy(pdu->caData + TRANSFER_ID_OFFSET, &t.id, sizeof(t.id));
        memcpy(pdu->caMsg, pathBytes.constData(), size_t(pathBytes.size()));
        t.state = Running;
        ++m_activeDownloads;
        cli.seedMsg(pdu);
    } else { // GroupDownload
        QByteArray payload;
        groupAppendU16(payload, GROUP_PROTOCOL_VERSION);
        groupAppendU16(payload, GROUP_ENVELOPE_LENGTH);
        groupAppendU64(payload, t.id);            // request_id
        groupAppendU64(payload, t.groupId);
        groupAppendU64(payload, t.groupFileId);
        PDU* pdu = mkPDU(uint(payload.size()));
        pdu->uiType = ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST;
        memcpy(pdu->caData + TRANSFER_ID_OFFSET, &t.id, sizeof(t.id));
        memcpy(pdu->caMsg, payload.constData(), size_t(payload.size()));
        t.state = Running;
        ++m_activeDownloads;
        cli.seedMsg(pdu);
    }
    emit tasksChanged();
}

void TransferManager::finishTask(Task& t, State s, const QString& reason)
{
    if (t.isUpload()) {
        if (t.state == Running || t.state == Hashing) {
            if (m_activeUploads > 0) --m_activeUploads;
        }
        if (t.uploader) {
            t.uploader->deleteLater();
            t.uploader = nullptr;
        }
    } else {
        if (t.state == Running && m_activeDownloads > 0) --m_activeDownloads;
        closeRecv(t);
    }
    t.state = s;
    if (!reason.isEmpty()) t.failReason = reason;
    emit tasksChanged();
    pump();
}

void TransferManager::closeRecv(Task& t)
{
    if (t.recvFile) {
        t.recvFile->close();
        delete t.recvFile;
        t.recvFile = nullptr;
    }
}

// 客户目录/yyyy-MM/ 下按 (N) 递推去重（沿用既有落盘语义，§5.4 验收 3）
bool TransferManager::beginReceiving(Task& t, const QString& fileName)
{
    const QString safeName = fileName.isEmpty() ? QStringLiteral("download.bin") : fileName;
    File* f = Index::getinstance().getFile();
    if (f == nullptr) return false;
    const QString monthDirPath = QString("%1/%2").arg(f->m_strUserPath)
            .arg(QDate::currentDate().toString("yyyy-MM"));
    QDir dir;
    if (!dir.mkpath(monthDirPath)) {
        t.failReason = QStringLiteral("创建下载目录失败");
        return false;
    }
    QString savePath = QDir(monthDirPath).filePath(safeName);
    const QFileInfo saveInfo(savePath);
    const QString baseName = saveInfo.completeBaseName();
    const QString suffix = saveInfo.suffix();
    int index = 1;
    while (QFileInfo::exists(savePath)) {
        const QString candidate = suffix.isEmpty()
                ? QString("%1(%2)").arg(baseName).arg(index)
                : QString("%1(%2).%3").arg(baseName).arg(index).arg(suffix);
        savePath = QDir(monthDirPath).filePath(candidate);
        ++index;
    }
    closeRecv(t);
    t.recvFile = new QFile(savePath);
    if (!t.recvFile->open(QIODevice::WriteOnly)) {
        delete t.recvFile;
        t.recvFile = nullptr;
        t.failReason = QStringLiteral("无法创建下载文件");
        return false;
    }
    t.savePath = savePath;
    t.displayName = safeName;
    return true;
}

TransferManager::Task* TransferManager::taskByTransferId(quint64 id)
{
    const quint64 key = stableKeyOf(id);
    if (key == 0) return nullptr;
    return taskByKey(key);
}

void TransferManager::onUploadInitRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    bool ret = false;
    qint64 resumeOffset = 0;
    uint magic = 0;
    memcpy(&ret, pdu->caData, sizeof(ret));
    memcpy(&magic, pdu->caData + UPLOAD_RESUME_MAGIC_OFFSET, sizeof(magic));
    if (magic == UPLOAD_RESUME_MAGIC) {
        memcpy(&resumeOffset, pdu->caData + UPLOAD_RESUME_OFFSET, sizeof(resumeOffset));
    }
    if (!ret) {
        // AUD-2 修复（design §3.3 / 验收 5）：INIT 被拒 = 合法的"继续排队"信号，不是失败。
        if (t->state != Running) return;   // 迟到响应（任务已被暂停/取消）：忽略，名额此前已归还
        if (m_activeUploads > 0) --m_activeUploads;   // 归还本任务在 startTask 计入的名额
        if (m_activeUploads > 0) {
            // 仍有在途上传：其结束事件（finishTask→pump）驱动本任务重试；此处不立即重发，避免自旋。
            // 摘要已由首次 97 判为未命中，重试直接走 29（与 resume() 同语义）。
            t->state = Queued;
            t->needFastcheck = false;
            t->failReason.clear();
            m_uploadQueue.append(t->key);
            emit tasksChanged();
            return;
        }
        // 无在途上传可释放名额：该拒绝不会自愈，收口为失败（避免无限静默排队）
        t->state = Queued;   // 名额已归还，避免 finishTask 二次扣减
        finishTask(*t, Failed, QStringLiteral("服务端拒绝上传初始化"));
        return;
    }
    t->uploader = new Uploader(id, t->localPath, resumeOffset, this);
    connect(t->uploader, &Uploader::seedMsg, &Client::getinstance(), &Client::seedMsg);
    const quint64 uploadKey = t->key;
    connect(t->uploader, &Uploader::errnoMsg, this, [this, uploadKey](const QString& msg) {
        Task* tt = taskByKey(uploadKey);
        if (tt) finishTask(*tt, Failed, msg);
    });
    connect(t->uploader, &Uploader::finished, this, [this, uploadKey](bool ok) {
        Task* tt = taskByKey(uploadKey);
        if (!tt) return;
        if (ok) finishTask(*tt, Done);
        else finishTask(*tt, Failed, QStringLiteral("上传失败"));
    });
    t->uploader->start();
    emit tasksChanged();
}

void TransferManager::onUploadDataRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr || t->uploader == nullptr) return;
    bool ret = true, complete = false;
    qint64 nextOffset = 0;
    uint magic = 0;
    memcpy(&magic, pdu->caData + UPLOAD_RESUME_MAGIC_OFFSET, sizeof(magic));
    if (magic == UPLOAD_RESUME_MAGIC) {
        memcpy(&ret, pdu->caData, sizeof(ret));
        memcpy(&nextOffset, pdu->caData + UPLOAD_ACK_NEXT_OFFSET, sizeof(nextOffset));
        memcpy(&complete, pdu->caData + UPLOAD_ACK_COMPLETE_OFFSET, sizeof(complete));
    }
    t->doneBytes = nextOffset;
    t->uploader->acknowledge(ret, nextOffset, complete);
    emit tasksChanged();
}

void TransferManager::onGroupUploadInitRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    quint32 code = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || !groupReadU32(payload, offset, code)
            || version != GROUP_PROTOCOL_VERSION || length != GROUP_ENVELOPE_LENGTH) {
        finishTask(*t, Failed, QStringLiteral("群上传响应非法"));
        return;
    }
    if (code != 0) {
        finishTask(*t, Failed, QStringLiteral("群上传拒绝（%1）").arg(qint32(code)));
        return;
    }
    if (payload.size() < GROUP_FILE_INIT_RESP_NAME_OFFSET) {
        finishTask(*t, Failed, QStringLiteral("群上传响应截断"));
        return;
    }
    auto readU64At = [&](int at) -> quint64 {
        quint64 v = 0;
        for (int i = 0; i < 8; ++i) v |= quint64(quint8(payload.at(at + i))) << (8 * i);
        return v;
    };
    const bool hit = (quint8(payload.at(GROUP_FILE_INIT_RESP_HIT_OFFSET)) != 0);
    const qint64 nextOffset = qint64(readU64At(GROUP_FILE_INIT_RESP_NEXT_OFFSET));
    if (hit) {
        finishTask(*t, Done);
        return;
    }
    t->uploader = new Uploader(id, t->localPath, nextOffset, this);
    connect(t->uploader, &Uploader::seedMsg, &Client::getinstance(), &Client::seedMsg);
    const quint64 groupUploadKey = t->key;
    connect(t->uploader, &Uploader::errnoMsg, this, [this, groupUploadKey](const QString& msg) {
        Task* tt = taskByKey(groupUploadKey);
        if (tt) finishTask(*tt, Failed, msg);
    });
    connect(t->uploader, &Uploader::finished, this, [this, groupUploadKey](bool ok) {
        Task* tt = taskByKey(groupUploadKey);
        if (!tt) return;
        if (ok) finishTask(*tt, Done);
        else finishTask(*tt, Failed, QStringLiteral("群上传失败"));
    });
    t->uploader->start();
    emit tasksChanged();
}

void TransferManager::onDownloadRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    bool ret = false;
    qint64 fileSize = 0;
    memcpy(&ret, pdu->caData, sizeof(ret));
    memcpy(&fileSize, pdu->caData + sizeof(bool), sizeof(fileSize));
    QString fileName = QString::fromUtf8(pdu->caData + 32);
    if (fileName.isEmpty()) fileName = QStringLiteral("download.bin");
    if (!ret) {
        finishTask(*t, Failed, QStringLiteral("服务端拒绝下载请求"));
        return;
    }
    t->totalBytes = fileSize;
    t->doneBytes = 0;
    if (!beginReceiving(*t, fileName)) {
        finishTask(*t, Failed, t->failReason);
        return;
    }
    emit tasksChanged();
}

void TransferManager::onGroupDownloadRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    if (pdu->uiMsgLen < 36) {
        finishTask(*t, Failed, QStringLiteral("群下载响应截断"));
        return;
    }
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    quint32 code = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || !groupReadU32(payload, offset, code)
            || version != GROUP_PROTOCOL_VERSION || length != GROUP_ENVELOPE_LENGTH) {
        finishTask(*t, Failed, QStringLiteral("群下载响应非法"));
        return;
    }
    if (code != 0) {
        finishTask(*t, Failed, QStringLiteral("群文件下载失败（%1）").arg(qint32(code)));
        return;
    }
    quint64 sizeBytes = 0;
    quint32 nameLen = 0;
    QByteArray nameBytes;
    if (!groupReadU64(payload, offset, sizeBytes) || !groupReadU32(payload, offset, nameLen)
            || !groupReadString(payload, offset, nameBytes)) {
        finishTask(*t, Failed, QStringLiteral("群下载元数据截断"));
        return;
    }
    t->totalBytes = qint64(sizeBytes);
    t->doneBytes = 0;
    if (!beginReceiving(*t, QString::fromUtf8(nameBytes))) {
        finishTask(*t, Failed, t->failReason);
        return;
    }
    emit tasksChanged();
}

void TransferManager::onDownloadData(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr || t->recvFile == nullptr || !t->recvFile->isOpen()) return;
    const qint64 written = t->recvFile->write(pdu->caMsg, pdu->uiMsgLen);
    if (written != qint64(pdu->uiMsgLen)) {
        qCWarning(lcCli) << "写入下载数据块失败 id=" << id;
        return;
    }
    t->doneBytes += written;
    emit tasksChanged();
}

void TransferManager::onDownloadFinish(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    bool success = false;
    memcpy(&success, pdu->caData, sizeof(success));
    const QString savePath = t->savePath;
    if (!success && !savePath.isEmpty()) {
        QFile::remove(savePath);
    }
    finishTask(*t, success ? Done : Failed, success ? QString() : QStringLiteral("下载失败"));
}

void TransferManager::sendPause(quint64 id)
{
    PDU* pdu = mkPDU();
    pdu->uiType = ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST;
    memcpy(pdu->caData + TRANSFER_ID_OFFSET, &id, sizeof(id));
    Client::getinstance().seedMsg(pdu);
}

// 暂停/取消共用 107（服务端处置相同，区别纯在客户端）
void TransferManager::sendAbortIfActive(Task& t)
{
    if (t.isUpload() && t.state == Running) {
        sendPause(t.id);
    }
}

void TransferManager::onAbortRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    bool ret = false;
    memcpy(&ret, pdu->caData, sizeof(ret));
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    // 暂停/取消已在本地落定，108 仅确认服务端已释放槽位（幂等，无需二次动作）
    qCDebug(lcCli) << "[transfer] 108 释放确认 id=" << id << " ret=" << ret;
}

void TransferManager::pause(quint64 key)
{
    Task* t = taskByKey(key);
    if (t == nullptr || !t->isUpload()) return;
    if (t->state == Queued || t->state == Hashing) {
        m_uploadQueue.removeOne(key);
        m_fastcheckQueue.removeOne(key);
        if (m_fastcheckInFlight == key) {
            m_fastcheckInFlight = 0;   // 98 到达时视为无归属，丢弃
            pumpFastcheck();
        }
        if (t->state == Hashing && m_activeUploads > 0) --m_activeUploads;
        t->state = Paused;
        emit tasksChanged();
        return;
    }
    if (t->state != Running) return;
    sendAbortIfActive(*t);
    // 停发 + 释放本地名额（服务端槽位由 107 释放；TCP 顺序保证 107 先于后续 INIT 到达）
    if (t->uploader) {
        t->uploader->deleteLater();
        t->uploader = nullptr;
    }
    if (m_activeUploads > 0) --m_activeUploads;
    t->state = Paused;
    emit tasksChanged();
    pump();
}

void TransferManager::resume(quint64 key)
{
    Task* t = taskByKey(key);
    if (t == nullptr || !t->isUpload()) return;
    if (t->state != Paused && t->state != Failed) return;
    // S3：以新 id 重走 INIT，服务端由 (owner|parent|display|size) 派生同一 staging → 续传
    t->id = allocId();
    m_idToKey.insert(t->id, key);
    t->failReason.clear();
    t->state = Queued;
    if (t->kind == PersonalUpload) {
        t->needFastcheck = false;   // 续传不再做秒传预检查（staging 已在服务端）
    }
    m_uploadQueue.append(key);
    emit tasksChanged();
    pump();
}

void TransferManager::cancel(quint64 key)
{
    Task* t = taskByKey(key);
    if (t == nullptr || !t->isUpload()) return;
    if (t->state == Running) {
        sendAbortIfActive(*t);
        if (t->uploader) {
            t->uploader->deleteLater();
            t->uploader = nullptr;
        }
        if (m_activeUploads > 0) --m_activeUploads;
    } else if (t->state == Queued) {
        m_uploadQueue.removeOne(key);
    } else if (t->state == Hashing) {
        if (m_activeUploads > 0) --m_activeUploads;
    }
    m_uploadQueue.removeOne(key);
    m_fastcheckQueue.removeOne(key);
    if (m_fastcheckInFlight == key) {
        m_fastcheckInFlight = 0;   // 98 到达时视为无归属，丢弃
    }
    m_idToKey.remove(t->id);
    m_tasks.remove(key);
    emit tasksChanged();
    pumpFastcheck();
    pump();
}

void TransferManager::retry(quint64 key)
{
    Task* t = taskByKey(key);
    if (t == nullptr) return;
    if (t->isUpload()) {
        resume(key);
        return;
    }
    if (t->state != Failed) return;
    t->id = allocId();
    m_idToKey.insert(t->id, key);
    t->failReason.clear();
    t->state = Queued;
    m_downloadQueue.append(key);
    emit tasksChanged();
    pump();
}

void TransferManager::showPanel()
{
    if (m_panel == nullptr) {
        m_panel = new TransferTaskPanel();
        connect(this, &TransferManager::tasksChanged,
                static_cast<TransferTaskPanel*>(m_panel), &TransferTaskPanel::refresh);
    }
    m_panel->show();
    m_panel->raise();
    m_panel->activateWindow();
    static_cast<TransferTaskPanel*>(m_panel)->refresh();
}

// ===================== TransferTaskPanel =====================

TransferTaskPanel::TransferTaskPanel(QWidget* parent)
    : QWidget(parent),
      m_table(new QTableWidget(this)),
      m_pauseBtn(new QPushButton(QStringLiteral("暂停"), this)),
      m_resumeBtn(new QPushButton(QStringLiteral("继续"), this)),
      m_cancelBtn(new QPushButton(QStringLiteral("取消"), this)),
      m_retryBtn(new QPushButton(QStringLiteral("重试"), this)),
      m_hint(new QLabel(QStringLiteral("双击传输行不可用；暂停/继续/取消仅对上传任务生效。"), this))
{
    setWindowTitle(QStringLiteral("传输任务"));
    resize(680, 360);

    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
                {QStringLiteral("文件"), QStringLiteral("方向"), QStringLiteral("进度"),
                 QStringLiteral("状态"), QStringLiteral("失败原因")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto* toolbar = new QHBoxLayout;
    toolbar->addWidget(m_pauseBtn);
    toolbar->addWidget(m_resumeBtn);
    toolbar->addWidget(m_cancelBtn);
    toolbar->addWidget(m_retryBtn);
    toolbar->addStretch();

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_hint);
    layout->addLayout(toolbar);
    layout->addWidget(m_table);

    connect(m_pauseBtn, &QPushButton::clicked, this, &TransferTaskPanel::onPause);
    connect(m_resumeBtn, &QPushButton::clicked, this, &TransferTaskPanel::onResume);
    connect(m_cancelBtn, &QPushButton::clicked, this, &TransferTaskPanel::onCancel);
    connect(m_retryBtn, &QPushButton::clicked, this, &TransferTaskPanel::onRetry);
    refresh();
}

quint64 TransferTaskPanel::selectedKey() const
{
    const int row = m_table->currentRow();
    if (row < 0) return 0;
    QTableWidgetItem* item = m_table->item(row, 0);
    if (item == nullptr) return 0;
    return item->data(Qt::UserRole).toULongLong();
}

void TransferTaskPanel::refresh()
{
    TransferManager& mgr = TransferManager::instance();
    const QList<quint64> keys = mgr.taskKeys();
    m_table->setRowCount(int(keys.size()));
    int row = 0;
    for (quint64 key : keys) {
        TransferManager::Task* t = mgr.taskByKey(key);
        if (t == nullptr) continue;
        QString dir;
        switch (t->kind) {
        case TransferManager::PersonalUpload:   dir = QStringLiteral("个人上传"); break;
        case TransferManager::GroupUpload:      dir = QStringLiteral("群上传"); break;
        case TransferManager::PersonalDownload: dir = QStringLiteral("个人下载"); break;
        case TransferManager::GroupDownload:    dir = QStringLiteral("群下载"); break;
        }
        QString progress = QStringLiteral("-");
        if (t->totalBytes > 0) {
            progress = QStringLiteral("%1%").arg(int(double(t->doneBytes) * 100.0 / double(t->totalBytes)));
        } else if (!t->isUpload() && t->state == TransferManager::Done) {
            progress = QStringLiteral("100%");
        }
        const QString stateText = TransferManager::stateText(t->state);
        auto* nameItem = new QTableWidgetItem(t->displayName);
        nameItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(key));
        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, new QTableWidgetItem(dir));
        m_table->setItem(row, 2, new QTableWidgetItem(progress));
        m_table->setItem(row, 3, new QTableWidgetItem(stateText));
        m_table->setItem(row, 4, new QTableWidgetItem(t->failReason));
        ++row;
    }
    m_table->setRowCount(row);
}

void TransferTaskPanel::onPause()
{
    const quint64 key = selectedKey();
    if (key != 0) TransferManager::instance().pause(key);
}

void TransferTaskPanel::onResume()
{
    const quint64 key = selectedKey();
    if (key != 0) TransferManager::instance().resume(key);
}

void TransferTaskPanel::onCancel()
{
    const quint64 key = selectedKey();
    if (key != 0) TransferManager::instance().cancel(key);
}

void TransferTaskPanel::onRetry()
{
    const quint64 key = selectedKey();
    if (key != 0) TransferManager::instance().retry(key);
}
