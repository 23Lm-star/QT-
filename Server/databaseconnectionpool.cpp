#include "logging.h"
#include "databaseconnectionpool.h"

#include <QDebug>
#include <QThread>

// 连接池实现：所有状态变更都在 m_mutex 内完成；QSqlDatabase 的 open/close 也在锁内执行，
// 因此并发 acquire 不会创建同名连接或突破全局上限。

// 默认构造 = 空句柄（获取失败的统一表示），析构不产生任何归还动作
PooledSqlConnection::PooledSqlConnection()
{
}

PooledSqlConnection::PooledSqlConnection(const QString &connectionName)
    : m_connectionName(connectionName)
{
}

// 移动构造：转移连接名所有权，源句柄置空以免析构时误归还
PooledSqlConnection::PooledSqlConnection(PooledSqlConnection &&other)
    : m_connectionName(other.m_connectionName)
{
    other.m_connectionName.clear();
}

// 析构即归还：所有使用点依赖 RAII 保证异常/提前返回时连接不泄漏
PooledSqlConnection::~PooledSqlConnection()
{
    release();
}

// 移动赋值：先归还自己原有连接，再接管对方；自赋值直接跳过
PooledSqlConnection &PooledSqlConnection::operator=(PooledSqlConnection &&other)
{
    if (this != &other) {
        release();
        m_connectionName = other.m_connectionName;
        other.m_connectionName.clear();
    }
    return *this;
}

// 三重校验：句柄非空、Qt 仍注册该连接名、连接处于 open
bool PooledSqlConnection::isValid() const
{
    return !m_connectionName.isEmpty()
            && QSqlDatabase::contains(m_connectionName)
            && QSqlDatabase::database(m_connectionName).isOpen();
}

// 按连接名取 Qt 连接；连接名已被 close 移除时返回默认构造对象（后续 exec 会失败）
QSqlDatabase PooledSqlConnection::database() const
{
    if (m_connectionName.isEmpty() || !QSqlDatabase::contains(m_connectionName)) {
        return QSqlDatabase();
    }
    return QSqlDatabase::database(m_connectionName);
}

// 透传底层连接最近一次错误，句柄失效时返回默认 QSqlError
QSqlError PooledSqlConnection::lastError() const
{
    return database().lastError();
}

// 归还后立即清空连接名，保证析构/移动赋值重复调用不会二次入队
void PooledSqlConnection::release()
{
    if (m_connectionName.isEmpty()) {
        return;
    }

    DatabaseConnectionPool::instance().release(m_connectionName);
    m_connectionName.clear();
}

// C++11 函数内静态变量：首次调用时线程安全地构造，进程退出时析构（关闭全部连接）
DatabaseConnectionPool &DatabaseConnectionPool::instance()
{
    static DatabaseConnectionPool pool;
    return pool;
}

// 默认参数与驱动名在此固定；init 之前 acquire 一律失败（m_inited=false）
DatabaseConnectionPool::DatabaseConnectionPool()
    : m_createdCount(0),
      m_nextConnectionId(0),
      m_maxConnectionCount(8),
      m_inited(false),
      m_driverName("QMYSQL"),
      m_port(3306)
{
}

DatabaseConnectionPool::~DatabaseConnectionPool()
{
    close();
}

// 仅保存参数，不建连；后续 acquire 才会按需 open。最大值钳到 >=1，避免 0 导致永远取不到连接
void DatabaseConnectionPool::init(const QString &hostName,
                                  int port,
                                  const QString &userName,
                                  const QString &password,
                                  const QString &databaseName,
                                  int maxConnectionCount)
{
    QMutexLocker locker(&m_mutex);
    m_hostName = hostName;
    m_port = port;
    m_userName = userName;
    m_password = password;
    m_databaseName = databaseName;
    m_maxConnectionCount = qMax(1, maxConnectionCount);
    m_inited = true;
}

// 借一条连接做可用性探测；作用域结束自动归还，不会长期占用
bool DatabaseConnectionPool::testConnection()
{
    PooledSqlConnection connection = acquire();
    return connection.isValid();
}

// 分配顺序：未 init 直接失败 → 空闲队列（掉线重开）→ 新建至上限 → 耗尽返回空句柄。
// 返回空句柄时调用方应视为数据库不可用，而不是重试等待。
PooledSqlConnection DatabaseConnectionPool::acquire()
{
    QMutexLocker locker(&m_mutex);
    if (!m_inited) {
        qCDebug(lcDb) << "database connection pool is not initialized";
        return PooledSqlConnection();
    }

    // 优先复用空闲连接：连接名已失效则丢弃；空闲期间被服务端断开则尝试重开
    while (!m_idleConnectionNames.isEmpty()) {
        const QString connectionName = m_idleConnectionNames.dequeue();
        if (!QSqlDatabase::contains(connectionName)) {
            continue;
        }

        QSqlDatabase db = QSqlDatabase::database(connectionName);
        if (!db.isOpen() && !db.open()) {
            qCWarning(lcDb) << "reopen mysql connection failed" << db.lastError().text();
            continue;
        }

        return PooledSqlConnection(connectionName);
    }

    if (m_createdCount >= m_maxConnectionCount) {
        qCDebug(lcDb) << "database connection pool exhausted";
        return PooledSqlConnection();
    }

    // 未达上限：新建连接；open 失败不计数、不留注册项
    const QString connectionName = createConnectionName();
    if (!openConnection(connectionName)) {
        return PooledSqlConnection();
    }

    ++m_createdCount;
    m_allConnectionNames.insert(connectionName);
    return PooledSqlConnection(connectionName);
}

// 关闭全部连接：先把状态清零，再逐个 close/removeDatabase；调用后池不可用，需重新 init
void DatabaseConnectionPool::close()
{
    QMutexLocker locker(&m_mutex);
    const QStringList connectionNames = m_allConnectionNames.values();
    m_idleConnectionNames.clear();
    m_allConnectionNames.clear();
    m_createdCount = 0;

    for (const QString &connectionName : connectionNames) {
        if (!QSqlDatabase::contains(connectionName)) {
            continue;
        }

        // 取连接时 open=false 避免隐式重连；先置空局部引用再 removeDatabase，
        // 否则 Qt 会警告仍有 QSqlDatabase 对象引用该连接
        QSqlDatabase db = QSqlDatabase::database(connectionName, false);
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }

    m_inited = false;
}

// 连接名只增不复用；自增在锁内，保证进程内唯一
QString DatabaseConnectionPool::createConnectionName()
{
    return QString("mysql_pool_%1").arg(++m_nextConnectionId);
}

// 注册并打开连接；open 失败时先释放局部引用再移除注册项，返回 false
bool DatabaseConnectionPool::openConnection(const QString &connectionName)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(m_driverName, connectionName);
    db.setHostName(m_hostName);
    db.setPort(m_port);
    db.setUserName(m_userName);
    db.setPassword(m_password);
    db.setDatabaseName(m_databaseName);

    if (db.open()) {
        return true;
    }

    qCWarning(lcDb) << "open mysql connection failed" << db.lastError().text();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return false;
}

// 归还只入队、不关闭：连接保持 open 供下次复用。
// 非池内登记的连接名（重复归还/伪造）不产生任何状态变化。
void DatabaseConnectionPool::release(const QString &connectionName)
{
    QMutexLocker locker(&m_mutex);
    if (!m_allConnectionNames.contains(connectionName)) {
        return;
    }

    m_idleConnectionNames.enqueue(connectionName);
}
