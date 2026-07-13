#ifndef UPLOADER_H
#define UPLOADER_H

#include "protocol.h"

#include <QObject>

class Uploader : public QObject
{
    Q_OBJECT
public:
    QString strUploadPath;
    qint64 m_resumeOffset;

    Uploader(const QString& strUploadPath, qint64 resumeOffset=0);
    void start();
public slots:
    void uploadFile();
signals:
    void seedMsg(PDU* pdu);
    void errnoMsg(const QString& msg);
    void finshUpload();
};

#endif // UPLOADER_H
