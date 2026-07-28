#include "operatedb.h"
#include "databaseconnectionpool.h"
#include "server.h"

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

OperateDB::~OperateDB()
{
}

OperateDB &OperateDB::instance()
{
    static OperateDB s;
    return s;
}

bool OperateDB::handleRegit(char *strname, char *strpwd)
{
    if(strname==nullptr||strpwd==nullptr){
        return false;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ?");
    q.addBindValue(QString::fromUtf8(strname));
    qDebug()<<"query user exist for:"<<QString::fromUtf8(strname);
    if(!q.exec()){
        qDebug()<<"query user exist failed:"<<q.lastError().text();
        return false;
    }
    if(q.next()){
        return false;
    }

    q.prepare("insert into user_info(name,pwd) values(?,?)");
    q.addBindValue(QString::fromUtf8(strname));
    q.addBindValue(QString::fromUtf8(strpwd));
    qDebug()<<"insert new user:"<<QString::fromUtf8(strname);
    return q.exec();
}

int OperateDB::handleLogin(char *strname, char *strpwd)
{
    if(strname==nullptr||strpwd==nullptr){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return 0;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    QString nameParam = QString::fromUtf8(strname);
    QString pwdParam = QString::fromUtf8(strpwd);
    qDebug()<<"[DIAG-DB] handleLogin: name="<<nameParam<<" name_hex="<<nameParam.toUtf8().toHex()<<" name_len="<<nameParam.size();
    qDebug()<<"[DIAG-DB] handleLogin: pwd_hex="<<pwdParam<<" pwd_len="<<pwdParam.size();
    
    // 先查用户是否存在（只看 name）
    QSqlQuery check(db);
    check.prepare("SELECT pwd, HEX(pwd), CHAR_LENGTH(pwd) FROM user_info WHERE name = ?");
    check.addBindValue(nameParam);
    if(check.exec() && check.next()){
        qDebug()<<"[DIAG-DB] DB中该用户存在! pwd="<<check.value(0).toString()<<" HEX="<<check.value(1).toString()<<" LEN="<<check.value(2).toInt();
    } else {
        qDebug()<<"[DIAG-DB] 用户 ["<<nameParam<<"] 根本不存在于数据库中!";
    }
    
    q.prepare("select * from user_info where name = ? and pwd = ?");
    q.addBindValue(nameParam);
    q.addBindValue(pwdParam);
    qDebug()<<"login select for:"<<QString::fromUtf8(strname);
    if(!q.exec()){
        qDebug()<<"login select failed:"<<q.lastError().text();
        return 0;
    }
    if(!q.next()){
        qDebug()<<"user name or password not found";
        return -1;
    }

    q.prepare("update user_info set online = 1 where name = ? and pwd = ?");
    q.addBindValue(QString::fromUtf8(strname));
    q.addBindValue(QString::fromUtf8(strpwd));
    qDebug()<<"login update online for:"<<QString::fromUtf8(strname);
    if (!q.exec()) {
        qCritical() << "UPDATE online status failed:" << q.lastError();
        return 0;
    }
    return 1;
}

void OperateDB::handleOffLine(const char *strname)
{
    if(strname==nullptr){
        return ;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("update user_info set online = 0 where name = ?");
    q.addBindValue(QString::fromUtf8(strname));
    qDebug()<<"offline update for:"<<QString::fromUtf8(strname);
    q.exec();
}

int OperateDB::handleFindUser(const char *userName)
{
    if(userName==nullptr){
        return -1;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ?");
    q.addBindValue(QString::fromUtf8(userName));
    qDebug()<<"find user:"<<QString::fromUtf8(userName);
    q.exec();
    if(q.next()){
        return q.value(3).toInt();
    }
    return 2;
}

QStringList OperateDB::handleOnlineUsers(const QString &strname)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return QStringList();
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // 查询在线且不是自己，且不在好友表中的用户
    q.prepare(R"(SELECT name FROM user_info 
                 WHERE online = 1 AND name != ? 
                 AND name NOT IN (
                     SELECT friendName FROM friend WHERE name = ?
                     UNION
                     SELECT name FROM friend WHERE friendName = ?
                 ))");
    q.addBindValue(strname);
    q.addBindValue(strname);
    q.addBindValue(strname);
    qDebug()<<"online users query for:"<<strname;
    q.exec();

    QStringList ret;
    while(q.next())
    {
        ret.append(q.value(0).toString());
    }
    return ret;
}

int OperateDB::handleAddFriend(char *usrname, char *tarname)
{
    if(usrname==nullptr||tarname==nullptr){
        return -1;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ? and online = 1");
    q.addBindValue(QString::fromUtf8(tarname));
    q.exec();
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

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    
    // 开启事务
    db.transaction();
    QSqlQuery q(db);

    // 使用两个名字字段存储好友关系
    q.prepare("INSERT INTO friend (name, friendName) VALUES (?, ?)");
    q.addBindValue(QString::fromUtf8(usrname));
    q.addBindValue(QString::fromUtf8(tarname));
    if(!q.exec()) {
        db.rollback();
        return 0;
    }

    if(!db.commit()) {
        db.rollback();
        return 0;
    }
    
    qDebug()<<"AddFriendFin:"<<QString::fromUtf8(usrname)<<"<->"<<QString::fromUtf8(tarname);
    return 1;
}

QStringList OperateDB::handleFlushFriend(const QString &strname)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return QStringList();
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // 查询 name=strname 时的 friendName，以及 friendName=strname 时的 name
    q.prepare(R"(SELECT friendName FROM friend WHERE name = ? 
                 UNION 
                 SELECT name FROM friend WHERE friendName = ?)");
    q.addBindValue(strname);
    q.addBindValue(strname);
    q.exec();

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

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    
    // 开启事务
    db.transaction();
    QSqlQuery q(db);

    // 删除双向好友记录
    q.prepare("DELETE FROM friend WHERE (name = ? AND friendName = ?) OR (name = ? AND friendName = ?)");
    q.addBindValue(QString::fromUtf8(userName));
    q.addBindValue(QString::fromUtf8(tarName));
    q.addBindValue(QString::fromUtf8(tarName));
    q.addBindValue(QString::fromUtf8(userName));
    if(!q.exec()) {
        db.rollback();
        return false;
    }

    if(!db.commit()) {
        db.rollback();
        return false;
    }
    
    qDebug()<<"deletefriend:"<<QString::fromUtf8(userName)<<"-"<<QString::fromUtf8(tarName);
    return true;
}

bool OperateDB::seedchatMsg(char *seedName, char *revName, QString msg, QString seedTime)
{
    if(seedName==nullptr||revName==nullptr){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ? and online = 1");
    q.addBindValue(QString::fromUtf8(revName));
    bool ans=1;
    ans&=q.exec();
    ans&=q.next();
    qDebug()<<"select online ans:"<<ans<<" for:"<<QString::fromUtf8(revName);

    q.prepare(R"(insert into message_info(seed_name,rev_name,msg,issuccess,seed_time)
        values(?,?,?,?,?))");
    q.addBindValue(QString::fromUtf8(seedName));
    q.addBindValue(QString::fromUtf8(revName));
    q.addBindValue(msg);
    q.addBindValue(ans ? 1 : 0);
    q.addBindValue(seedTime);
    bool execRet=q.exec();
    qDebug()<<"chat msg insert:"<<execRet;
    return ans;
}

QStringList OperateDB::getchatMsg(char *seedName, char *revName)
{
    QStringList ret;
    if(seedName==nullptr||revName==nullptr){
        return ret;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ret;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select msg,seed_time from message_info where seed_name = ? and rev_name = ? and issuccess = 0");
    q.addBindValue(QString::fromUtf8(seedName));
    q.addBindValue(QString::fromUtf8(revName));
    qDebug()<<"getchatMsg for:"<<QString::fromUtf8(seedName)<<"->"<<QString::fromUtf8(revName);
    if(!q.exec()){
        return ret;
    }
    while(q.next()){
        QString retItem1(q.value(0).toString());
        QString retItem2(q.value(1).toString());
        ret.append(retItem1);
        ret.append(retItem2);
    }

    q.prepare("update message_info set issuccess = 1 where seed_name = ? and rev_name = ?");
    q.addBindValue(QString::fromUtf8(seedName));
    q.addBindValue(QString::fromUtf8(revName));
    qDebug()<<"update msg issuccess=1 ret:"<<q.exec();
    return ret;
}

bool OperateDB::handleGetUserInfo(const char *userName, QString &outName, int &outOnline)
{
    if(userName==nullptr){
        return false;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);
    //使用预编译查询防止 SQL 注入
    q.prepare("select name, online from user_info where name = ?");
    q.addBindValue(QString::fromUtf8(userName));
    if(!q.exec() || !q.next()){
        return false;
    }
    outName = q.value(0).toString();
    outOnline = q.value(1).toInt();
    return true;
}

bool OperateDB::handleLogoff(const char* name, const char* pwd)
{
    if(name==nullptr||pwd==nullptr){
        return false;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();

    db.transaction();
    QSqlQuery q(db);

    // 1. 验证用户名和密码
    q.prepare("SELECT * FROM user_info WHERE name = ? AND pwd = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(QString::fromUtf8(pwd));
    if(!q.exec() || !q.next()){
        qDebug()<<"logoff: user/pwd mismatch or user not found:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 2. 删除 message_info 中涉及该用户的记录
    q.prepare("DELETE FROM message_info WHERE seed_name = ? OR rev_name = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qDebug()<<"logoff: failed to delete messages for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 3. 删除 friend 表中涉及该用户的所有记录（双向）
    q.prepare("DELETE FROM friend WHERE name = ? OR friendName = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qDebug()<<"logoff: failed to delete friend records for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 4. 删除 user_info 表中的用户记录
    q.prepare("DELETE FROM user_info WHERE name = ? AND pwd = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(QString::fromUtf8(pwd));
    if(!q.exec()){
        qDebug()<<"logoff: failed to delete user_info for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    if(!db.commit()){
        qDebug()<<"logoff: commit failed for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    qDebug()<<"logoff: successfully deleted user:"<<QString::fromUtf8(name);
    return true;
}

void OperateDB::connect()
{
    Server& server = Server::getinstance();
    DatabaseConnectionPool::instance().init(
        server.m_dbHost,
        server.m_dbPort,
        server.m_dbUser,
        server.m_dbPassword,
        server.m_dbName
    );
    if(DatabaseConnectionPool::instance().testConnection()){
        qDebug()<<"connect database success"<<endl;
    }
    else {
        qDebug()<<"connect database failed";
    }
}

OperateDB::OperateDB(QObject *parent) : QObject(parent)
{
}
