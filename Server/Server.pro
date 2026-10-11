QT       += core gui network sql

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11 utf8_source

# console: 必须有。缺省时 MinGW 目标按 GUI 子系统链接（-Wl,-subsystem,windows），进程没有
# stdout/stderr，Qt 的 qDebug/qWarning 全部走 OutputDebugString —— 服务端运行日志既进不了
# 重定向文件也进不了管道，套件（group-002 p7_slow_conn_dropped）无法断言丢包日志。
# 加回控制台子系统后 `dist/Server.exe > log 2>&1` 与 Popen(stdout=FILE) 均可捕获。
CONFIG += console

# Deploy destination policy:
#   Debug   -> stay beside Qt Creator's run config (green triangle works as-is)
#   Release -> converge into repo-root dist/ (the single deployable artifact)
CONFIG(debug, debug|release) {
    DESTDIR =
} else {
    DESTDIR = $$PWD/../dist
}

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    databaseconnectionpool.cpp \
    fileobjectstore.cpp \
    logging.cpp \
    main.cpp \
    msghandler.cpp \
    mytcpserver.cpp \
    mytcpsocket.cpp \
    operatedb.cpp \
    protocol.cpp \
    reclaimscheduler.cpp \
    resumableuploadsession.cpp \
    server.cpp \
    sharelinkservice.cpp \
    uploadwritetask.cpp \
    ThreadCache.cpp \
    CentralCache.cpp \
    PageCache.cpp \
    ConCurrentAlloc.cpp \
    memalloc.cpp

HEADERS += \
    databaseconnectionpool.h \
    fileobjectstore.h \
    logging.h \
    msghandler.h \
    mytcpserver.h \
    mytcpsocket.h \
    operatedb.h \
    protocol.h \
    reclaimscheduler.h \
    resumableuploadsession.h \
    server.h \
    sharelinkservice.h \
    uploadwritetask.h \
    Common.h \
    ThreadCache.h \
    CentralCache.h \
    PageCache.h \
    ObjectPool.h \
    RadixTreeHash.h \
    ConCurrentAlloc.h \
    memalloc.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    config.qrc

DISTFILES +=
