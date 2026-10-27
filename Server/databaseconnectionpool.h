#ifndef DATABASECONNECTIONPOOL_H
#define DATABASECONNECTIONPOOL_H

#include <QHash>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtGlobal>

// 进程级 MySQL 连接池：按需创建、全局空闲队列复用、上限封顶。
// 连接不绑定线程（弃用了早先的按线程分区），acquire/release 由同一互斥锁保护；
// 借出的连接在同一时刻只应被一个线程使用，RAII 句柄析构即归还。

class DatabaseConnectionPool;

// 连接池借出句柄（仅移动语义）：析构或移动赋值时自动把连接名归还池中。
// 空句柄（默认构造/移动后）表示获取失败，isValid()=false。
class PooledSqlConnection
{
public:
    PooledSqlConnection();
    PooledSqlConnection(PooledSqlConnection &&other);
    ~PooledSqlConnection();

    PooledSqlConnection &operator=(PooledSqlConnection &&other);

    PooledSqlConnection(const PooledSqlConnection &) = delete;
    PooledSqlConnection &operator=(const PooledSqlConnection &) = delete;

    // 连接名仍注册且 QSqlDatabase 处于 open 状态才为 true
    bool isValid() const;
    // 取连接对象；句柄无效时返回默认 QSqlDatabase（调用前应先 isValid）
    QSqlDatabase database() const;
    // 最近一次 SQL 错误（透传 QSqlDatabase::lastError）
    QSqlError lastError() const;

private:
    friend class DatabaseConnectionPool;

    // 仅连接池可构造，保证句柄一定对应池内登记的连接
    explicit PooledSqlConnection(const QString &connectionName);
    // 幂等归还：清空连接名后重复调用无副作用
    void release();

    QString m_connectionName;
};

// 单例连接池：内部延迟建连，首次 acquire 时才真正 open；
// 连接耗尽（达上限）时返回空句柄而非阻塞等待，由调用方按"数据库不可用"处理。
class DatabaseConnectionPool
{
public:
    // 进程内唯一实例（函数内静态对象，线程安全的懒初始化）
    static DatabaseConnectionPool &instance();

    // 保存连接参数并标记可用；maxConnectionCount 至少为 1（不在此处建连）
    void init(const QString &hostName,
              int port,
              const QString &userName,
              const QString &password,
              const QString &databaseName,
              int maxConnectionCount = 8);
    // 借一条连接试探可用性，结果随 RAII 句柄析构归还
    bool testConnection();
    // 取连接：先复用空闲队列（掉线自动重开），否则在未达上限时新建；失败返回空句柄
    PooledSqlConnection acquire();
    // 关闭并移除所有已创建连接，重置计数；调用后需重新 init
    void close();

private:
    friend class PooledSqlConnection;

    DatabaseConnectionPool();
    ~DatabaseConnectionPool();

    DatabaseConnectionPool(const DatabaseConnectionPool &) = delete;
    DatabaseConnectionPool &operator=(const DatabaseConnectionPool &) = delete;

    // 生成唯一连接名 mysql_pool_<n>（调用方须持锁）
    QString createConnectionName();
    // 按已保存参数 addDatabase+open；失败时移除注册项，避免 Qt 连接名泄漏
    bool openConnection(const QString &connectionName);
    // 归还连接名到空闲队列；非池内登记的连接名被忽略（调用方须持锁）
    void release(const QString &connectionName);

    QMutex m_mutex;
    QQueue<QString> m_idleConnectionNames;   // 全局共享的空闲连接队列（不再按线程分区）
    QSet<QString> m_allConnectionNames;      // 所有创建过的连接名，用于统一释放
    int m_createdCount;                       // 已创建连接数（受全局上限约束）
    int m_nextConnectionId;                   // 连接名序号
    int m_maxConnectionCount;                 // 全局最大连接数
    // init 成功后才允许 acquire
    bool m_inited;

    // 固定 QMYSQL
    QString m_driverName;
    QString m_hostName;
    int m_port;
    QString m_userName;
    QString m_password;
    QString m_databaseName;
};

#endif // DATABASECONNECTIONPOOL_H
