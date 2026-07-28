#ifndef DATABASECONNECTIONPOOL_H
#define DATABASECONNECTIONPOOL_H

#include <QHash>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtGlobal>

class DatabaseConnectionPool;

class PooledSqlConnection
{
public:
    PooledSqlConnection();
    PooledSqlConnection(PooledSqlConnection &&other);
    ~PooledSqlConnection();

    PooledSqlConnection &operator=(PooledSqlConnection &&other);

    PooledSqlConnection(const PooledSqlConnection &) = delete;
    PooledSqlConnection &operator=(const PooledSqlConnection &) = delete;

    bool isValid() const;
    QSqlDatabase database() const;
    QSqlError lastError() const;

private:
    friend class DatabaseConnectionPool;

    explicit PooledSqlConnection(const QString &connectionName);
    void release();

    QString m_connectionName;
};

class DatabaseConnectionPool
{
public:
    static DatabaseConnectionPool &instance();

    void init(const QString &hostName,
              int port,
              const QString &userName,
              const QString &password,
              const QString &databaseName,
              int maxConnectionCountPerThread = 8);
    bool testConnection();
    PooledSqlConnection acquire();
    void close();

private:
    friend class PooledSqlConnection;

    struct ThreadConnections
    {
        QQueue<QString> idleConnectionNames;
        int createdCount = 0;
    };

    DatabaseConnectionPool();
    ~DatabaseConnectionPool();

    DatabaseConnectionPool(const DatabaseConnectionPool &) = delete;
    DatabaseConnectionPool &operator=(const DatabaseConnectionPool &) = delete;

    QString createConnectionName(Qt::HANDLE threadId);
    bool openConnection(const QString &connectionName);
    void release(const QString &connectionName);

    QMutex m_mutex;
    QHash<Qt::HANDLE, ThreadConnections> m_connections;
    QHash<QString, Qt::HANDLE> m_connectionThreads;
    int m_nextConnectionId;
    int m_maxConnectionCountPerThread;
    bool m_inited;

    QString m_driverName;
    QString m_hostName;
    int m_port;
    QString m_userName;
    QString m_password;
    QString m_databaseName;
};

#endif // DATABASECONNECTIONPOOL_H
