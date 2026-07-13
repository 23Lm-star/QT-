#include "operatedb.h"
#include <QDebug>
#include <QSqlQuery>
#include <QTime>
#include <QtSql/QSqlError>
OperateDB::~OperateDB()
{
    m_db.close();
}
OperateDB &OperateDB::instance()
{
    static OperateDB s;
    return s;
}
//数据库用户注册操作函数
bool OperateDB::handleRegit(char *strname, char *strpwd)
{
    //检查 用户名和密码 是否为空
    if(strname==nullptr||strpwd==nullptr){
        return false;
    }
    //操作 查询sql语句，查询用户是否存在
    QString sql_select=QString("select * from user_info where name='%1'").arg(strname);
    qDebug()<<"查询用户是否存在"<<sql_select<<endl;
    //用于执行sql语句,并处理查询结果
    QSqlQuery q;
    if(!q.exec(sql_select)){
        return false;
    }
    if(q.next()){
        return false;
    }
    QString sql_insert=QString("insert into user_info(name,pwd) values('%1','%2')").arg(strname).arg(strpwd);
    qDebug()<<"插入一条新用户"<<sql_insert<<endl;
    return q.exec(sql_insert);
}
//处理登录请求
int OperateDB::handleLogin(char *strname, char *strpwd)
{
    if(strname==nullptr||strpwd==nullptr){
        return 0;
    }
    //查找 用户名和密码
    QString sql_Login_select=QString("select * from user_info where name = '%1' and pwd = '%2' ").arg(strname).arg(strpwd);
    QSqlQuery q;
    qDebug()<<"查询用户名和密码是否正确 sql"<< sql_Login_select<<endl;
    //若是执行sql语句过程失败
    if(!q.exec(sql_Login_select)){
        qDebug()<<"状态异常"<<endl;
        return 0;
    }
    //若是没有查询结果（用户名和密码有一个不存在）
    if(!q.next()){
        qDebug()<<"用户名和密码有一个不存在"<<endl;
        return -1;
    }
    QString sql_Login_set=QString("update user_info set online = 1 where name = '%1' and pwd = '%2' ").arg(strname).arg(strpwd);
    qDebug()<<"更新用户online为1 sql"<< sql_Login_set<<endl;
    //将 数据库中 用户的登录状态设置为 已登录
    return q.exec(sql_Login_set);
}

void OperateDB::handleOffLine(const char *strname)
{
    if(strname==nullptr){
        return ;
    }
    QSqlQuery q;
    QString sql_Login_set=QString("update user_info set online = 0 where name = '%1'").arg(strname);
    qDebug()<<"登录用户下线 online为0 sql"<< sql_Login_set<<endl;
    q.exec(sql_Login_set);
}

int OperateDB::handleFindUser(const char *userName)
{
    if(userName==nullptr){
        return -1;//查询用户 失败
    }
    QString sql_findUser=QString("select * from user_info where name = '%1' ").arg(userName);
    qDebug()<<"查询用户online字段 sql"<<sql_findUser<<endl;
    QSqlQuery q;
    q.exec(sql_findUser);
    if(q.next()){//有这个用户
        //第几个字段
        return q.value(3).toInt();//是否在线
    }
    return 2;//没有这个用户
}

QStringList OperateDB::handleOnlineUsers(const QString &strname)
{
    QString sql=QString(R"(select name from user_info
                        where id not in (select friend_id from friend  where user_id =(select id from user_info where name ='%1') ))").arg(strname);
    QSqlQuery q;
    qDebug()<<"查询所有用户 sql"<<sql<<endl;
    q.exec(sql);
    QStringList ret;
    ret.clear();
    while(q.next()){
        ret.append(q.value(0).toString());
    }
    return ret;
}

int OperateDB::handleAddFriend(char *usrname, char *tarname)
{
    if(usrname==nullptr||tarname==nullptr){
        return -1;
    }
    QString sql=QString("select * from user_info where name ='%1' and online = 1").arg(tarname);
    QSqlQuery q;
    q.exec(sql);
    if(q.next()){
        return 1;
    }
    return 0;
}

int OperateDB::handleAddFriendFin(char *usrname, char *tarname)
{
    if(usrname==nullptr||tarname==nullptr){
        return -1;
    }
    QString sql=QString(R"(
                        insert into friend(user_id,friend_id)
                        values((select id from user_info where name='%1'),(select id from user_info where name='%2')),
                        ((select id from user_info where name='%2'),(select id from user_info where name='%1'))
                        )").arg(usrname).arg(tarname).arg(tarname).arg(usrname);
    qDebug()<<"AddFriendFin sql"<<sql<<endl;
    QSqlQuery q;
    return q.exec(sql);
}

QStringList OperateDB::handleFlushFriend(const QString &strname)
{
    QString sql=QString(R"(select name from user_info where
                        id in (select friend_id from friend
                        where user_id = (select id from user_info where name ='%1')))").arg(strname);
    QSqlQuery q;
    q.exec(sql);
    QStringList ret;
    while(q.next()){
        ret.append(q.value(0).toString());
    }
    return ret;
}

bool OperateDB::handleDeleteFriend(char *userName, char *tarName)
{
    if(userName==nullptr||tarName==nullptr){
        return 0;
    }
    QString sql=QString(R"(
                        delete from friend where(
                        user_id=(select id from user_info where name='%1')and
                        friend_id=(select id from user_info where name='%2'))
                        or
                        (user_id=(select id from user_info where name='%3')
                        and
                        friend_id=(select id from user_info where name='%4'))
                        )").arg(userName).arg(tarName).arg(tarName).arg(userName);
    qDebug()<<"deletefriend ret sql"<<sql<<endl;
    QSqlQuery q;
    return q.exec(sql);
}

bool OperateDB::seedchatMsg(char *seedName, char *revName, QString msg, QString seedTime)
{
    if(seedName==nullptr||revName==nullptr){
        return 0;
    }
    //查询收信人是否在线
    QString sql=QString("select * from user_info where name ='%1' and online = 1").arg(revName);
    QSqlQuery q;
    //记录是否能够转发：一个查询在线失败 一个 查询出不在线
    bool ans=1;
    ans&=q.exec(sql);
    ans&=q.next();
    qDebug()<<"select online ans:"<<ans <<" "<<sql;
    //记录信息
    sql=QString(R"(insert into message_info(seed_name,rev_name,msg,issuccess,seed_time)
                values('%1','%2','%3',%4,'%5')
                )").arg(seedName).arg(revName).arg(msg).arg(ans).arg(seedTime);
    qDebug()<<"title msg ans:"<<q.exec(sql) <<" "<<sql;
    return ans;
}

QStringList OperateDB::getchatMsg(char *seedName, char *revName)
{
    QStringList ret;
    if(seedName==nullptr||revName==nullptr){
        return ret;
    }
    QString sql=QString("select msg,seed_time from message_info where seed_name ='%1' and rev_name = '%2' and issuccess = 0").arg(seedName).arg(revName);
    qDebug()<<"getchatMsg sql"<<sql<<endl;
    QSqlQuery q;
    if(!q.exec(sql)){
        return ret;
    }
    while(q.next()){
        QString retItem1(q.value(0).toString());
        QString retItem2(q.value(1).toString());
        ret.append(retItem1);
        ret.append(retItem2);
    }
    sql=QString("update message_info set issuccess = 1 where seed_name ='%1' and rev_name = '%2'").arg(seedName).arg(revName);
    qDebug()<<"update msg issuccess=1 ret:"<< q.exec(sql);
    return ret;
}

//连接数据库
void OperateDB::connect()
{
    //设定数据库的信息
    m_db.setPort(3306);
    m_db.setHostName("localhost");
    m_db.setUserName("root");
    m_db.setPassword("123456");
    m_db.setDatabaseName("mydb");
    //查看链接数据库是否成功
    if(m_db.open()){
        qDebug()<<"连接数据库成功"<<endl;
    }
    else {
        qDebug()<<"连接数据库失败"<<m_db.lastError().text();
    }
}



OperateDB::OperateDB(QObject *parent) : QObject(parent)
{
    m_db=QSqlDatabase::addDatabase("QMYSQL");
}

