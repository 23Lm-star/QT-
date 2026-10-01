#include "logging.h"
#include "client.h"
#include "file.h"
#include "index.h"
#include "ui_file.h"
#include "uploader.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <qinputdialog.h>
#include <qmessagebox.h>
#include <cstring>
#include <QDialog>

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
    qCDebug(lcCli)<<"创建用户目录:"<<m_strUserPath.section('/', -1)<<" "<<dir.mkdir(m_strUserPath)<<endl;
    qCDebug(lcCli)<<"创建用户今日目录:"<<QDate::currentDate().toString("yyyy-MM")<<" "<<dir.mkdir(m_strUserPath+"/"+QString(QDate::currentDate().toString("yyyy-MM")))<<endl;
    m_shareFile=new ShareFile;
    
    // 初始化右键菜单
    ui->file_LW->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->file_LW, &QListWidget::customContextMenuRequested, this, &File::onFileListContextMenu);
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
    if(m_uploader){
        return;
    }
    m_uploader=new Uploader(m_strUploadPath,resumeOffset,this);
    connect(m_uploader,&Uploader::seedMsg,&Client::getinstance(),&Client::seedMsg);
    connect(m_uploader,&Uploader::errnoMsg,this,&File::errnoMsg);
    connect(m_uploader,&Uploader::finished,this,[this](bool success){
        if(success){
            flushfile();
        }
        m_uploader->deleteLater();
        m_uploader=nullptr;
    });
    m_uploader->start();
}

void File::acknowledgeUploadChunk(bool success, qint64 nextOffset, bool complete)
{
    if(m_uploader){
        m_uploader->acknowledge(success,nextOffset,complete);
    }
}

void File::updateCurrfile(QList<FileInfo *> qFileinfolist){
    // 步骤1：释放旧数据内存
    foreach(FileInfo* fileInfos,m_fileInfoList){
        delete fileInfos;
    }
    // 步骤2：接管新数据（File 拥有所有权）
    m_fileInfoList=qFileinfolist;
    qCDebug(lcCli) << "[File] updateCurrfile: 缓存" << m_fileInfoList.size() << "个文件条目";
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
        qCDebug(lcCli)<<"pFileInfo->caName:"<<m_fileInfoList[i]->caName<<" qFileinfolist[i]->uiType"<<m_fileInfoList[i]->uiType<<" ";
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
        qCDebug(lcCli) << "[File] scheduleUiRefresh: 页面不可见，延迟到 showEvent";
        return;
    }
    // 已排队则跳过（合并去重）
    if (m_uiRefreshQueued) {
        return;
    }
    m_uiRefreshQueued = true;
    qCDebug(lcCli) << "[File] scheduleUiRefresh: 排入一次 UI 刷新";
    QTimer::singleShot(0, this, [this]() {
        m_uiRefreshQueued = false;
        // 回调执行时重新检查可见性与待刷新标志
        if (isVisible() && m_pendingUiRefresh) {
            qCDebug(lcCli) << "[File] UI 刷新：开始重建";
            rebuildFileList();
            qCDebug(lcCli) << "[File] UI 刷新：完成";
        }
    });
}

void File::showEvent(QShowEvent *event){
    QWidget::showEvent(event);
    qCDebug(lcCli) << "[File] showEvent: 页面变为可见，pending=" << m_pendingUiRefresh;
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
     qCDebug(lcCli)<<"pdu->caData:"<<pdu->caData<<"pdu->caMsg"<<"<path suppressed>"<<endl;
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
     qCDebug(lcCli)<<"确认删除文件结果："<<del_ret<<endl;
     if(del_ret==QMessageBox::No){
         qCDebug(lcCli)<<"取消删除文件"<<endl;
         return;
     }
     QString PathName(QString(this->m_strCurPath+"/"+fileName->text()));
     uint fileType=0;
     qCDebug(lcCli)<<"uitype:"<<fileType<<"pdu->caMsg"<<"<path suppressed>"<<endl;
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
     qCDebug(lcCli)<<"uitype:"<<fileType<<"pdu->caMsg"<<"<path suppressed>"<<endl;
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
    qCDebug(lcCli)<<"旧文件名"<<pdu->caData<<"新文件名"<<pdu->caData+32<<" 当前路径"<<"<path suppressed>"<<endl;
    Client::getinstance().seedMsg(pdu);
}
void File::on_move_PB_clicked()
{
    QListWidgetItem* item = ui->file_LW->currentItem();
    if(!item){
        QMessageBox::information(this,"提示","请选择要移动的文件或文件夹");
        return;
    }
    QString srcRel = selectedFileRelativePath(item);

    if(m_moveDlg){
        delete m_moveDlg;
        m_moveDlg = nullptr;
    }
    m_moveDlg = new FolderSelectDialog(this);
    connect(m_moveDlg, &FolderSelectDialog::listDirRequested, this, &File::requestListDir);
    m_moveDlg->browse(""); // 触发首次目录列表加载（连接已建立）
    if(m_moveDlg->exec() == QDialog::Accepted){
        QString dstRel = m_moveDlg->selectedRel();
        // 构造请求：caData=源文件名（短名）；caMsg=srcRel + '\0' + dstRel
        QByteArray srcRelBytes = srcRel.toUtf8();
        QByteArray dstRelBytes = dstRel.toUtf8();
        QByteArray msg;
        msg.append(srcRelBytes);
        msg.append('\0');
        msg.append(dstRelBytes);
        msg.append('\0');

        PDU* pdu = mkPDU(msg.size());
        pdu->uiType = ENUM_MSG_TYPE_MOVEFILE_REQUEST;
        copyFixedField(pdu->caData, 32, item->text().toUtf8());
        memcpy(pdu->caMsg, msg.constData(), msg.size());
        Client::getinstance().seedMsg(pdu);
    }
    delete m_moveDlg;
    m_moveDlg = nullptr;
}

void File::requestListDir(const QString &rel)
{
    QByteArray relBytes = rel.toUtf8();
    QByteArray msg;
    msg.append(relBytes);
    msg.append('\0');
    PDU* pdu = mkPDU(msg.size());
    pdu->uiType = ENUM_MSG_TYPE_LISTDIR_REQUEST;
    memcpy(pdu->caMsg, msg.constData(), msg.size());
    Client::getinstance().seedMsg(pdu);
}

void File::handleListDirRespond(PDU *pdu)
{
    if(m_moveDlg){
        m_moveDlg->handleListDir(pdu);
    }
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
void File::on_upload_PB_clicked()
{
    //获取要上传的文件路径，并记录下来
    m_strUploadPath=QFileDialog::getOpenFileName();
    qCDebug(lcCli)<<"上传文件源路径"<<m_strUploadPath.section('/', -1)<<endl;
    //取出要上传的文件名
    int index=m_strUploadPath.lastIndexOf('/');
    QString uploadName=m_strUploadPath.right(m_strUploadPath.size()-index-1);//取出文件名
    if(!isValidFileName(uploadName)){
        QMessageBox::information(this,"提示","文件名不合法");
        return;
    }
    // REQ-EXT-FILE-002 §4：秒传预检查——先流式计算 SHA-256（1MiB 分块，内存 O(1)，验收6），
    // 再发 FASTCHECK 请求；命中则服务端零正文建条目，未命中回 nextOffset 走既有分块路径。
    QFile file(m_strUploadPath);
    if(!file.open(QIODevice::ReadOnly)){
        QMessageBox::information(this,"提示","无法读取所选文件");
        return;
    }
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    while(!file.atEnd()){
        const QByteArray chunk=file.read(1024*1024);
        if(chunk.isEmpty() && file.error()!=QFile::NoError){
            QMessageBox::information(this,"提示","读取文件失败");
            return;
        }
        hasher.addData(chunk);
    }
    const qint64 fileSize=file.size();
    file.close();
    const QByteArray shaHex=hasher.result().toHex();          // 64 hex（小写）

    // 请求级唯一标识（32hex）：同一文件一次上传的键固定，重试即可命中服务端幂等短路
    const QByteArray reqId=QUuid::createUuid().toRfc4122().toHex();

    const QByteArray nameBytes=uploadName.toUtf8();
    const QByteArray dirBytes=m_strCurPath.toUtf8();
    QByteArray msg;
    msg.append(shaHex);
    msg.append(nameBytes);
    msg.append(dirBytes);
    msg.append(reqId);

    PDU* pdu=mkPDU(uint(msg.size()));
    pdu->uiType=ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST;
    const uint version=FASTCHECK_VERSION;
    const uint magic=FASTCHECK_MAGIC;
    const uint shaLen=uint(FASTCHECK_SHA_HEX_LEN);
    const uint nameLen=uint(nameBytes.size());
    const uint dirLen=uint(dirBytes.size());
    const uint reqIdLen=uint(reqId.size());
    memcpy(pdu->caData+FASTCHECK_VERSION_OFFSET,&version,sizeof(version));
    memcpy(pdu->caData+FASTCHECK_MAGIC_OFFSET,&magic,sizeof(magic));
    memcpy(pdu->caData+FASTCHECK_FILE_SIZE_OFFSET,&fileSize,sizeof(fileSize));
    memcpy(pdu->caData+FASTCHECK_SHA_LEN_OFFSET,&shaLen,sizeof(shaLen));
    memcpy(pdu->caData+FASTCHECK_NAME_LEN_OFFSET,&nameLen,sizeof(nameLen));
    memcpy(pdu->caData+FASTCHECK_DIR_LEN_OFFSET,&dirLen,sizeof(dirLen));
    memcpy(pdu->caData+FASTCHECK_REQID_LEN_OFFSET,&reqIdLen,sizeof(reqIdLen));
    memcpy(pdu->caMsg,msg.constData(),size_t(msg.size()));
    qCDebug(lcCli)<<"秒传预检查 文件名"<<nameBytes.constData()<<"文件大小"<<fileSize<<endl;
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
        // 获取选中的文件信息，检查类型
        uint fileType = 1; // 默认值
        for(int i=0;i<this->m_fileInfoList.size();i++){
            if(strcmp(m_fileInfoList[i]->caName,listWidgeti->text().toStdString().c_str())==0){
                fileType=m_fileInfoList[i]->uiType;
                break;
            }
        }
        if (fileType == 0) {
            QMessageBox::information(this, "提示", "目录不可分享");
            return;
        }
        m_shareFile->show();
    }
}



void File::onFileListContextMenu(const QPoint &pos) {
    QListWidgetItem *item = ui->file_LW->itemAt(pos);
    if (!item) return;
    
    // 只对普通文件（非目录）显示菜单
    uint fileType = 1; // 默认普通文件
    for(int i=0;i<this->m_fileInfoList.size();i++){
        if(strcmp(m_fileInfoList[i]->caName,item->text().toStdString().c_str())==0){
            fileType=m_fileInfoList[i]->uiType;
            break;
        }
    }
    if (fileType == 0) return; // 目录
    
    QMenu menu;
    menu.addAction("获取分享链接", this, &File::onGetShareLink);
    menu.exec(ui->file_LW->viewport()->mapToGlobal(pos));
}

QString File::selectedFileRelativePath(QListWidgetItem *item)
{
    // 注意：绝不能对成员变量 m_strCurPath 做原地 remove，否则会污染当前路径，
    // 导致后续 flush/返回上级都发出错误路径（表现为列表刷出 Server 工程目录）。
    QString prefix = Client::getinstance().m_strRootPath + "/" + Client::getinstance().m_LoginName;
    QString relCur = m_strCurPath;   // 副本
    relCur.remove(prefix);
    QString relativePath = QString("%1/%2").arg(relCur).arg(item->text());
    if (relativePath.startsWith("/")) relativePath = relativePath.mid(1);
    // 处理多级重复拼接问题：确保移除后的部分不重复拼接
    if (relativePath.startsWith(m_strUserPath)) {
        relativePath = relativePath.mid(m_strUserPath.size());
        if (relativePath.startsWith("/")) relativePath = relativePath.mid(1);
    }
    return relativePath;
}

void File::onGetShareLink() {
    QListWidgetItem *item = ui->file_LW->currentItem();
    if (!item) return;
    QString relativePath = selectedFileRelativePath(item);
    PDU* pdu = mkPDU(relativePath.toUtf8().size() + 1);
    pdu->uiType = ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_REQUEST;
    copyFixedField(pdu->caData, 32, Client::getinstance().m_LoginName.toUtf8());
    memcpy(pdu->caMsg, relativePath.toUtf8().constData(), relativePath.toUtf8().size());
    Client::getinstance().seedMsg(pdu);
}

void File::on_genlink_PB_clicked()
{
    QListWidgetItem* item = ui->file_LW->currentItem();
    if (item == nullptr) {
        QMessageBox::information(this, "提示", "请选择要生成分享链接的文件");
        return;
    }
    // 仅普通文件可生成分享链接，文件夹不允许
    uint fileType = 1;
    for (int i = 0; i < this->m_fileInfoList.size(); i++) {
        if (QString::fromUtf8(m_fileInfoList[i]->caName) == item->text()) {
            fileType = m_fileInfoList[i]->uiType;
            break;
        }
    }
    if (fileType == 0) {
        QMessageBox::information(this, "提示", "文件夹不能生成分享链接");
        return;
    }
    QString relativePath = selectedFileRelativePath(item);
    PDU* pdu = mkPDU(relativePath.toUtf8().size() + 1);
    pdu->uiType = ENUM_MSG_TYPE_CREATE_PUBLINK_REQUEST;
    memcpy(pdu->caMsg, relativePath.toUtf8().constData(), relativePath.toUtf8().size());
    Client::getinstance().seedMsg(pdu);
}

void File::on_fetchshare_PB_clicked()
{
    QString text = ui->sharelink_LE->text().trimmed();
    if (text.isEmpty()) {
        QMessageBox::information(this, "提示", "请先粘贴分享链接");
        return;
    }
    // 兼容完整链接（cloud-disk://share/<token>）与纯 token 两种输入
    QString token = text;
    const QString prefix = "cloud-disk://share/";
    if (token.startsWith(prefix)) {
        token = token.mid(prefix.length()).trimmed();
    }
    if (token.isEmpty()) {
        QMessageBox::information(this, "提示", "分享链接格式不正确");
        return;
    }

    QByteArray tokenBytes = token.toUtf8();
    PDU* pdu = mkPDU(tokenBytes.size() + 1);
    pdu->uiType = ENUM_MSG_TYPE_RESOLVE_PUBLINK_REQUEST;
    memcpy(pdu->caMsg, tokenBytes.constData(), tokenBytes.size());
    Client::getinstance().seedMsg(pdu);
}
