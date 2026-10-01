#ifndef UPLOADER_H
#define UPLOADER_H

#include "protocol.h"

#include <QObject>
#include <QFile>

class Uploader : public QObject
{
    Q_OBJECT
public:
    QString strUploadPath;
    qint64 m_resumeOffset;

    Uploader(quint64 transferId, const QString& strUploadPath, qint64 resumeOffset=0, QObject* parent=nullptr);
    void start();
    quint64 transferId() const { return m_transferId; }
public slots:
    void acknowledge(bool success, qint64 nextOffset, bool complete);
signals:
    void seedMsg(PDU* pdu);
    void errnoMsg(const QString& msg);
    void finished(bool success);

private:
    void sendNextChunk();
    void finish(bool success);

    quint64 m_transferId;
    QFile m_file;
    qint64 m_nextOffset;
    bool m_waitingAck;
    bool m_active;
    bool m_finished;
};

#endif // UPLOADER_H
