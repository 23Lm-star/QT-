#include "client.h"
#include "file.h"
#include "index.h"
#include "ui_file.h"
#include "uploader.h"

#include <QByteArray>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <qinputdialog.h>
#include <qmessagebox.h>
#include <cstring>

namespace {
bool isValidFileName(const QString &name)
{
    QString trimmed=name.trimmed();
    if(trimmed.isEmpty() || trimmed!=name){
        return false;
    }
    if(name=="." || name==".." || name.toUtf8().size()>31){
        return false;
    }
    if(name.endsWith('.') || name.endsWith(' ')){
        return false;
    }

    const QString forbidden="<>:\"/\\|?*";
    for(const QChar &ch:name){
        if(ch.unicode()<32 || forbidden.contains(ch)){
            return false;
        }
    }

    QString base=name.section('.',0,0).toUpper();
    static const QStringList reserved={
        "CON","PRN","AUX","NUL",
        "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
        "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"
    };
    return !reserved.contains(base);
}

void copyFixedField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize - 1));
}
}

File::File(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::File)
{
    ui->setupUi(this);
    m_strUserPath=QString("%1/%2").arg(Client::getinstance().m_strRootPath).arg(Client::getinstance().m_LoginName);
    m_strCurPath=m_strUserPath;
    ui->currPath_LE->setText(m_strCurPath);
    //创建用户目录和资源目录,若是不存在会创建，存在就不创建了
    QDir dir;
    qDebug()<<"创建用户目录:"<<m_strUserPath<<" "<<dir.mkdir(m_strUserPath)<<endl;
    qDebug()<<"创建用户今日目录:"<<m_strUserPath+"/"+QString(QDate::currentDate().toString("yyyy-MM"))<<" "<<dir.mkdir(m_strUserPath+"/"+QString(QDate::currentDate().toString("yyyy-MM")))<<endl;
    m_shareFile=new ShareFile;
}

File::~File()
{
    delete m_shareFile;
    delete ui;

}

void File::flushfile()
{
    int curPathLen=m_strCurPath.toUtf8().toStdString().size();//记录当前路径长度(utf8编码)
    PDU* pdu=mkPDU(curPathLen+1);//留出一个空格来
    pdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_REQUEST;
    memcpy(pdu->caMsg,m_strCurPath.toUtf8().toStdString().c_str(),curPathLen);
    Client::getinstance().seedMsg(pdu);
}

void File::uploadfile(qint64 resumeOffset)
{
    Uploader * Uploaderi=new Uploader(m_strUploadPath, resumeOffset);
    //将上传文件里面的关于UI的语句全部使用信号槽的方式进行调用
    connect(Uploaderi,&Uploader::seedMsg,&Client::getinstance(),&Client::seedMsg,Qt::QueuedConnection);
    connect(Uploaderi,&Uploader::errnoMsg,this,&File::errnoMsg,Qt::QueuedConnection);
    Uploaderi->start();
    //delete Uploaderi;
}

void File::updateCurrfile(QList<FileInfo *> qFileinfolist){
    // 步骤1：释放旧数据内存
    foreach(FileInfo* fileInfos,m_fileInfoList){
        delete fileInfos;
    }
    // 步骤2：接管新数据（File 拥有所有权）
    m_fileInfoList=qFileinfolist;
    qDebug() << "[File] updateCurrfile: 缓存" << m_fileInfoList.size() << "个文件条目";
    // 步骤3：数据/UI 分离——仅标记待刷新，由 scheduleUiRefresh 统一调度
    m_pendingUiRefresh=true;
    scheduleUiRefresh();
}

void File::rebuildFileList(){
    ui->file_LW->clear();
    for(int i=0;i<m_fileInfoList.size();i++){
        QListWidgetItem*pItem=new QListWidgetItem;
        if(m_fileInfoList[i]->uiType==0){
            pItem->setIcon(QIcon(QPixmap(":/dir.png")));
        }
        else if(m_fileInfoList[i]->uiType==1){
            pItem->setIcon(QIcon(QPixmap(":/file.png")));
        }
        pItem->setText(m_fileInfoList[i]->caName);
        qDebug()<<"pFileInfo->caName:"<<m_fileInfoList[i]->caName<<" qFileinfolist[i]->uiType"<<m_fileInfoList[i]->uiType<<" ";
        ui->file_LW->addItem(pItem);
    }
    m_pendingUiRefresh=false;
}

void File::scheduleUiRefresh()
{
    // 无待处理数据则跳过
    if (!m_pendingUiRefresh) {
        return;
    }
    // 不可见时不排队，等 showEvent 触发
    if (!isVisible()) {
        qDebug() << "[File] scheduleUiRefresh: 页面不可见，延迟到 showEvent";
        return;
    }
    // 已排队则跳过（合并去重）
    if (m_uiRefreshQueued) {
        return;
    }
    m_uiRefreshQueued = true;
    qDebug() << "[File] scheduleUiRefresh: 排入一次 UI 刷新";
    QTimer::singleShot(0, this, [this]() {
        m_uiRefreshQueued = false;
        // 回调执行时重新检查可见性与待刷新标志
        if (isVisible() && m_pendingUiRefresh) {
            qDebug() << "[File] UI 刷新：开始重建";
            rebuildFileList();
            qDebug() << "[File] UI 刷新：完成";
        }
    });
}

void File::showEvent(QShowEvent *event){
    QWidget::showEvent(event);
    qDebug() << "[File] showEvent: 页面变为可见，pending=" << m_pendingUiRefresh;
    // 通过合并排队机制请求重建，不直接操作 QListWidget
    scheduleUiRefresh();
}

void File::flushCurrLE()
{
    ui->currPath_LE->setText(m_strCurPath);
}

void File::errnoMsg(const QString &Msg)
{
    QMessageBox::information(this,"提示",Msg);
}
//创建目录/文件
void File::on_credit_PB_clicked()
{
     QString strdirName=QInputDialog::getText(this,"新建文件夹","文件夹名");
     if(!isValidFileName(strdirName)){
         QMessageBox::information(this,"提示","文件夹名不合法");
         return;
     }
     uint dirSize=this->m_strCurPath.toUtf8().toStdString().size();
     PDU* pdu=mkPDU(dirSize+1);
     pdu->uiType=ENUM_MSG_TYPE_MKDIR_REQUEST;
     copyFixedField(pdu->caData,32,strdirName.toUtf8());
     memcpy(pdu->caMsg,m_strCurPath.toUtf8().toStdString().c_str(),dirSize);
     qDebug()<<"pdu->caData:"<<pdu->caData<<"pdu->caMsg"<<pdu->caMsg<<endl;
     Client::getinstance().seedMsg(pdu);
}

void File::on_flush_PB_clicked()
{
    flushfile();
}
//删除文件/目录
void File::on_del_PB_clicked()
{
     QListWidgetItem* fileName=ui->file_LW->currentItem();//获取到要删除的文件夹名称
     if(fileName==nullptr){
         return ;
     }
     int del_ret= QMessageBox::question(this,"提示",QString("你确定要删除文件%1吗").arg(fileName->text()));
     qDebug()<<"确认删除文件结果："<<del_ret<<endl;
     if(del_ret==QMessageBox::No){
         qDebug()<<"取消删除文件"<<endl;
         return;
     }
     QString PathName(QString(this->m_strCurPath+"/"+fileName->text()));
     uint fileType=0;
     qDebug()<<"uitype:"<<fileType<<"pdu->caMsg"<<PathName<<endl;
     for(int i=0;i<this->m_fileInfoList.size();i++){
         if(strcmp(m_fileInfoList[i]->caName,fileName->text().toStdString().c_str())==0){
             memcpy(&fileType,&m_fileInfoList[i]->uiType,sizeof(m_fileInfoList[i]->uiType));
             break;
         }
     }
     PDU* pdu=mkPDU(PathName.toStdString().size()+1);
     pdu->uiType=ENUM_MSG_TYPE_DELFILE_REQUEST;
     memcpy(pdu->caData,&fileType,sizeof(fileType));
     memcpy(pdu->caMsg,PathName.toStdString().c_str(),PathName.toStdString().size());
     qDebug()<<"uitype:"<<fileType<<"pdu->caMsg"<<pdu->caMsg<<endl;
     Client::getinstance().seedMsg(pdu);
}
//进入当前文件/目录
void File::on_file_LW_itemDoubleClicked(QListWidgetItem *item)
{
    QListWidgetItem* triName = ui->file_LW->currentItem();
    uint triType=0;
    for(int i=0;i<this->m_fileInfoList.size();i++){
        if(strcmp(m_fileInfoList[i]->caName,triName->text().toStdString().c_str())==0){
            memcpy(&triType,&m_fileInfoList[i]->uiType,sizeof(uint));
            break;
        }
    }
    if(triType==0){//若是选中文件为目录
        this->m_strCurPath+="/"+triName->text().toUtf8();//将目录切换到选中文件目录
        ui->currPath_LE->setText(m_strCurPath);
        //刷新文件
        PDU* pdu=mkPDU(m_strCurPath.toStdString().size()+1);
        pdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_REQUEST;
        memcpy(pdu->caMsg,this->m_strCurPath.toStdString().c_str(),this->m_strCurPath.toStdString().size());
        Client::getinstance().seedMsg(pdu);
    }
    else if(triType==1){

    }
}
//返回上一级目录
void File::on_back_PB_clicked()
{
    //若是当前已经是用户目录了，就不能继续返回上级目录了
    if(this->m_strCurPath==this->m_strUserPath){
        return;
    }
    int cnt=m_strCurPath.size()-1;
    for(int i=m_strCurPath.size()-1;i>=0;i--){
        if(m_strCurPath[i]=='/'){
            cnt=i;
            break;
        }
    }
    //将当前目录切换到上级目录，删除 /当前目录名
    m_strCurPath.remove(cnt,m_strCurPath.size()-cnt);
    ui->currPath_LE->setText(m_strCurPath);
    //刷新文件
    PDU* pdu=mkPDU(m_strCurPath.toStdString().size()+1);
    pdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_REQUEST;
    memcpy(pdu->caMsg,this->m_strCurPath.toStdString().c_str(),this->m_strCurPath.toStdString().size());
    Client::getinstance().seedMsg(pdu);
}

void File::on_rename_PB_clicked()
{
    QListWidgetItem* oldfileName=ui->file_LW->currentItem();
    if(oldfileName==nullptr){
        QMessageBox::information(this,"提示","请选择要重命名的文件");
        return ;
    }
    QString strfileName=QInputDialog::getText(this,"文件重命名","新文件名");
    if(strfileName==oldfileName->text()){//如果说当前新文件名和原来文件名相同的话就不需要修改
        return ;
    }
    if(!isValidFileName(strfileName)){//如果新文件名长度不合理不能修改
        QMessageBox::information(this,"提示","新文件名长度不合法");
        return;
    }
    //记录文件类型
    uint fileType=0;
    for(int i=0;i<this->m_fileInfoList.size();i++){
        if(strcmp(m_fileInfoList[i]->caName,oldfileName->text().toStdString().c_str())==0){
            fileType=m_fileInfoList[i]->uiType;
            break;
        }
    }
    //pdu->caMsg里面存入一个当前目录和文件类型
    PDU* pdu=mkPDU(sizeof(fileType)+this->m_strCurPath.toStdString().size()+1);
    pdu->uiType=ENUM_MSG_TYPE_RENAMEFILE_REQUEST;
    copyFixedField(pdu->caData,32,oldfileName->text().toUtf8());//旧文件名
    copyFixedField(pdu->caData+32,32,strfileName.toUtf8());//新文件名
    memcpy(pdu->caMsg,&fileType,sizeof(fileType));
    memcpy(pdu->caMsg+sizeof (fileType),this->m_strCurPath.toStdString().c_str(),this->m_strCurPath.toStdString().size());
    qDebug()<<"旧文件名"<<pdu->caData<<"新文件名"<<pdu->caData+32<<" 当前路径"<<pdu->caMsg+sizeof (fileType)<<endl;
    Client::getinstance().seedMsg(pdu);
}
//手动跳转路径
void File::on_inputPath_PB_clicked(){
    //目标路径
    QString tarPath=ui->currPath_LE->text();
    //目标路径长度(utf8)
    int tarPathLen=tarPath.toUtf8().toStdString().size();
    //构建pdu 长度为tarPathLen+1
    PDU* pdu=mkPDU(tarPathLen+1);
    pdu->uiType=ENUM_MSG_TYPE_CHANGEDIR_REQUEST;
    memcpy(pdu->caMsg,tarPath.toUtf8().toStdString().c_str(),tarPathLen);
    Client::getinstance().seedMsg(pdu);
}

void File::on_mkdir_PB_clicked()
{
    //获取要上传的文件路径，并记录下来
    m_strUploadPath=QFileDialog::getOpenFileName();
    qDebug()<<"上传文件源路径"<<m_strUploadPath<<endl;
    //取出要上传的文件名和文件大小
    int index=m_strUploadPath.lastIndexOf('/');
    QString uploadName=m_strUploadPath.right(m_strUploadPath.size()-index-1);//取出文件名
    if(!isValidFileName(uploadName)){
        QMessageBox::information(this,"提示","文件名不合法");
        return;
    }
    QFile file(m_strUploadPath);
    qint64 fileSize=file.size();//获取文件大小
    PDU* pdu=mkPDU(m_strCurPath.toUtf8().toStdString().size()+1);
    pdu->uiType=ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST;
    uint resumeMagic=UPLOAD_RESUME_MAGIC;
    copyFixedField(pdu->caData,32,uploadName.toUtf8());
    memcpy(pdu->caData+UPLOAD_FILE_SIZE_OFFSET,&fileSize,sizeof(fileSize));
    memcpy(pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
    memcpy(pdu->caMsg,m_strCurPath.toUtf8().toStdString().c_str(),m_strCurPath.toUtf8().toStdString().size());//当前路径
    qDebug()<<"文件名"<<pdu->caData<<"文件大小"<<fileSize<<" 上传路径"<<pdu->caMsg<<endl;
    Client::getinstance().seedMsg(pdu);
}

void File::on_download_PB_clicked()
{
    QListWidgetItem* fileItem=ui->file_LW->currentItem();
    if(fileItem==nullptr){
        QMessageBox::information(this,"提示","请选择要下载的文件");
        return;
    }

    uint fileType=0;
    bool found=false;
    for(int i=0;i<this->m_fileInfoList.size();i++){
        if(QString::fromUtf8(m_fileInfoList[i]->caName)==fileItem->text()){
            fileType=m_fileInfoList[i]->uiType;
            found=true;
            break;
        }
    }
    if(!found || fileType!=1){
        QMessageBox::information(this,"提示","只能下载普通文件");
        return;
    }

    QString filePath=QString("%1/%2").arg(this->m_strCurPath).arg(fileItem->text());
    QByteArray filePathBytes=filePath.toUtf8();
    PDU* pdu=mkPDU(filePathBytes.size()+1);
    pdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST;
    QByteArray fileNameBytes=fileItem->text().toUtf8();
    memcpy(pdu->caData,fileNameBytes.constData(),qMin(fileNameBytes.size(),31));
    memcpy(pdu->caMsg,filePathBytes.constData(),filePathBytes.size());
    Client::getinstance().seedMsg(pdu);

}

void File::on_share_PB_clicked()
{
    QListWidgetItem* listWidgeti=ui->file_LW->currentItem();
    if(listWidgeti==nullptr){
        return ;
    }
    m_shareFile->flusfriend();
    m_strShareFileName=listWidgeti->text();
    if(m_shareFile->isHidden()){
        m_shareFile->show();
    }
}


