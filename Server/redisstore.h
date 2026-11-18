#ifndef REDISSTORE_H
#define REDISSTORE_H

#include <QByteArray>
#include <QString>
#include <QStringList>

class RedisStore
{
public:
    static RedisStore& instance();
    bool enabled() const;
    bool setEx(const QString& key, int seconds, const QByteArray& value);
    bool get(const QString& key, QByteArray& value);
    bool del(const QString& key);
    bool incr(const QString& key, qint64& value);
    bool evalCompareDelete(const QString& key, const QByteArray& version);
    bool mget(const QStringList& keys, QList<QByteArray>& values);
private:
    RedisStore();
    RedisStore(const RedisStore&) = delete;
    RedisStore& operator=(const RedisStore&) = delete;
    bool command(const QList<QByteArray>& args, QByteArray& payload, char& type);
    bool parseResponse(const QByteArray& data, int& offset, QByteArray& payload, char& type);
    bool parseUrl();
    bool m_enabled = false;
    QString m_host;
    quint16 m_port = 6379;
    QByteArray m_password;
};

#endif
