#include "protocol.h"
#include "uploader.h"

#include <QFile>
#include <QDebug>
#include <qthread.h>
#include <cstring>
void Uploader::uploadFile()
{
    QFile uploadFile(this->strUploadPath);
    qDebug()<<"上传文件:"<<this->strUploadPath<<"文件大小"<<uploadFile.size()<<endl;
    if(!uploadFile.open(QIODevice::ReadOnly)){
        emit errnoMsg("上传文件失败");
        emit finshUpload();
        return ;
    }
    if(m_resumeOffset < 0){
        m_resumeOffset=0;
    }
    if(m_resumeOffset >= uploadFile.size()){
        qDebug()<<this->strUploadPath<<" already uploaded, offset"<<m_resumeOffset<<endl;
        uploadFile.close();
        emit finshUpload();
        return;
    }
    if(m_resumeOffset > 0 && !uploadFile.seek(m_resumeOffset)){
        emit errnoMsg("上传文件失败");
        uploadFile.close();
        emit finshUpload();
        return;
    }
    qint64 read_all=m_resumeOffset;
    while(1){
        //读取pdu对象
        PDU* pdu=mkPDU(4096);
        pdu->uiType=ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST;
        qint64 chunkOffset=read_all;
        uint resumeMagic=UPLOAD_RESUME_MAGIC;
        memcpy(pdu->caData+UPLOAD_CHUNK_OFFSET,&chunkOffset,sizeof(chunkOffset));
        memcpy(pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
        //读取数据到pdu->caMsg，并返回read_count
        qint64 read_count=uploadFile.read(pdu->caMsg,4096);
        if(read_count<0){
            emit errnoMsg("上传文件失败");
            break;
        }
        if(read_count==0){
            qDebug()<<this->strUploadPath<<" read end"<<endl;
            break;
        }
        read_all+=read_count;
        pdu->uiTotalLen=sizeof (PDU)+read_count;//更新pdu总大小
        pdu->uiMsgLen=read_count;//更新pdu->msg大小
        qDebug()<<"read_count"<<read_count<<" read_all"<<read_all<<" chunkOffset"<<chunkOffset<<endl;
        emit seedMsg(pdu);
    }
    uploadFile.close();
    emit finshUpload();
}

Uploader::Uploader(const QString &strUploadPath, qint64 resumeOffset)
{
    this->strUploadPath=strUploadPath;
    this->m_resumeOffset=resumeOffset;
}

void Uploader::start()
{
    //开启一个线程
    QThread *threadi=new QThread;
    //将当前uploader对象喂给线程
    this->moveToThread(threadi);
    //将线程的开启以及关闭、清理工作全部利用信号和槽函数关联起来
    connect(threadi,&QThread::started,this,&Uploader::uploadFile,Qt::QueuedConnection);
    connect(this,&Uploader::finshUpload,threadi,&QThread::quit,Qt::QueuedConnection);
    connect(threadi,&QThread::finished,threadi,&QThread::deleteLater,Qt::QueuedConnection);
    //开启这个线程的运行
    threadi->start();
}

