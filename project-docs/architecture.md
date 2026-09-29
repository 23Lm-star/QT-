# 总体架构与技术选型

> 生成依据：代码实现 + `docs/agent.md` + `docs/PROJECT_IMPLEMENTATION.md`
> 状态：初版，与当前实现一致

---

## 1. 系统架构总览

```
┌─────────────────────────────────────────────────────────────────────┐
│  Client (Qt GUI + QTcpSocket)                                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────────────┐ │
│  │ 登录/注册 │  │ 好友管理 │  │ 聊天界面 │  │ 文件管理           │ │
│  │ (client)  │  │ (friend) │  │ (chat)   │  │ (file/index/upload)│ │
│  └─────┬─────┘  └────┬─────┘  └────┬─────┘  └─────────┬──────────┘ │
│        └──────────────┴─────────────┴──────────────────┘            │
│                           │ MsgHandler 分发                          │
│                     ┌─────┴──────┐                                  │
│                     │  QTcpSocket │ (单连接)                         │
│                     └─────┬──────┘                                  │
└───────────────────────────┼──────────────────────────────────────────┘
                            │
                    ┌───────┴────────┐
                    │  自定义 PDU    │  (76B 包头 + 柔性数组)
                    │  TCP 二进制协议 │
                    └───────┬────────┘
                            │
┌───────────────────────────┼──────────────────────────────────────────┐
│  Server (Qt Console)      │                                          │
│  ┌────────────────────────┴──────────────────────┐                  │
│  │  MyTcpServer (QTcpServer, 单例)                │                  │
│  │  ├─ QThreadPool (max 8)                       │                  │
│  │  └─ QList<MyTcpSocket*> tcpsocketlist          │                  │
│  └────────────────────┬──────────────────────────┘                  │
│                       │                                              │
│  ┌────────────────────┴──────────────────────────┐                  │
│  │  MyTcpSocket (每个客户端连接)                   │                  │
│  │  ├─ recvmsg(): 粘包/半包处理                    │                  │
│  │  ├─ handlMsg(): 通过 MsgHandler 按类型分发      │                  │
│  │  └─ seedMsg(): 发送响应并 memfree               │                  │
│  └────────────────────┬──────────────────────────┘                  │
│                       │                                              │
│  ┌────────────────────┴──────────────────────────┐                  │
│  │  MsgHandler (每个Socket一个实例)                │                  │
│  │  注册/登录/好友/聊天/文件/验证码                 │                  │
│  └────────────────────┬──────────────────────────┘                  │
│                       │                                              │
│  ┌────────────────────┴──────────────────────────┐                  │
│  │  OperateDB (单例)                              │                  │
│  │  └── DatabaseConnectionPool (RAII 连接池)      │                  │
│  │       ├── MySQL: user_info / friend / message  │                  │
│  │       └── 按线程隔离，每线程上限 8 连接          │                  │
│  └────────────────────────────────────────────────┘                  │
│                                                                      │
│  ┌────────────────────────────────────────────────┐                  │
│  │  自研内存池 (TCMalloc 风格)                     │                  │
│  │  ThreadCache → CentralCache → PageCache        │                  │
│  │  对象上限 256KB, 页大小 8KB                     │                  │
│  └────────────────────────────────────────────────┘                  │
│                                                                      │
│  ┌────────────────────────────────────────────────┐                  │
│  │  文件系统                                      │                  │
│  │  {filesPath}/{用户名}/                          │                  │
│  │  支持: 上传/下载/删除/重命名/分享/移动/断点续传   │                  │
│  └────────────────────────────────────────────────┘                  │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 2. 技术栈

| 层次 | 技术选择 | 版本/说明 |
|------|----------|----------|
| 语言 | C++11/14 | `CONFIG += c++11` |
| GUI 框架 | Qt 5 Widgets | Qt 5.14.2 MinGW 64-bit |
| 网络 | QTcpSocket / QTcpServer | 自定义 TCP 二进制协议 |
| 数据库 | MySQL + QSqlDatabase | 通过自研连接池访问 |
| 构建 | qmake (.pro) | 客户端和服务端各一份 |
| 平台 | Windows (主) / Linux (可移植) | |

---

## 3. 通信协议

**自定义 PDU (Protocol Data Unit)**：

```
sizeof(PDU) = 76 字节 (uint=4B 平台)
┌──────────────┬──────────────┬──────────────┬──────────────────────┐
│ uiTotalLen   │  uiMsgLen    │  uiType      │  caData[64]          │
│ 4B           │  4B          │  4B          │  64B                 │
├──────────────┴──────────────┴──────────────┴──────────────────────┤
│ caMsg[] (柔性数组, 实际长度 = uiMsgLen)                             │
└────────────────────────────────────────────────────────────────────┘
```

- 枚举消息类型：43 种（注册/登录/好友/聊天/文件/验证码等）
- `caData[64]` 复用规则：前32B 和 后32B 根据消息类型承载不同参数
- 客户端：`malloc/free` 分配释放
- 服务端：`memalloc/memfree` 分配释放（走内存池）
- **禁止混用**两端分配/释放函数

---

## 4. 并发模型

| 角色 | 线程模型 |
|------|----------|
| 服务端监听 | 主线程（QApplication 事件循环） |
| 客户端连接处理 | QThreadPool（max 8），但实际 I/O 由 Socket 所在线程的事件循环驱动 |
| 文件上传 | `Uploader` 通过 `moveToThread` 在独立 QThread 运行 |
| 数据库访问 | 连接池按 `QThread::currentThreadId()` 隔离，每线程上限 8 连接 |

---

## 5. 设计模式

| 模式 | 应用位置 | 备注 |
|------|----------|------|
| 单例 | `Client`, `Server` | Meyer's Singleton |
| 单例 | `MyTcpServer`, `OperateDB` | 同上 |
| 单例 | `Index` (Client 主界面) | 同上 |
| 单例 | `CentralCache`, `PageCache` (内存池) | 同上 |
| 单例 | `DatabaseConnectionPool` | 同上 |
| 工厂 | `mkPDU()` | 构造协议数据单元 |
| 对象池 | `ObjectPool<Span>` | 内存池 Span 复用 |
| 观察者 | Qt 信号槽 | 连接、数据到达、上传进度 |
| RAII | `PooledSqlConnection` | 析构时自动归还数据库连接 |

---

## 6. 低耦合可靠性与应用内链接扩展（ADR-011，规划）

```text
ClientHeartbeat ──(空 PING/PONG PDU)──> MyTcpSocket
       │                                      │
       └── 只经 Client 的 socket 生命周期接入     └── 不访问 DB/业务模块

File ──> ShareLinkDialog ──(创建/解析链接 PDU)──> MsgHandler
                                                     │
                                                     v
                                             ShareLinkService ──> OperateDB（仅 share_link 窄接口）
                                                     │
                                                     └── 解析成功复用既有下载流
```

- `ClientHeartbeat` 是唯一客户端心跳定时器；不依赖或修改 File/Friend/Chat/ShareFile。
- 分享链接为已登录用户使用的 `cloud-disk://share/<opaque-token>` 访问凭证；不是 HTTP 匿名公网访问。
- `ShareLinkService` 只依赖文件根目录校验和 `OperateDB` 的新增窄接口；禁止改变既有好友分享、资源所有权和默认访问权限。
- 详见 `decisions/ADR-011-20260729-低耦合心跳与应用内分享链接.md`。

---

## 7. 关键依赖关系

```
Client → protocol.h (共用协议定义)
       → MsgHandler (响应分发)
       → Index (主界面单例, 持有 File/Friend/Chat)
       → Uploader (独立线程上传)

Server → protocol.h (共用协议定义)
       → MyTcpServer (监听+连接管理)
       → MyTcpSocket (单连接处理)
       → ClientTask (线程池包装, QRunnable)
       → MsgHandler (请求处理逻辑)
       → OperateDB (数据库访问)
       → DatabaseConnectionPool (连接池)
       → ResumableUploadSession (断点续传)
       → 内存池 (ThreadCache/CentralCache/PageCache/ConCurrentAlloc)
```

## 8. 日志分级（REQ-LOG-007）

- 全部 213 处日志调用改走 `QLoggingCategory`：服务端 5 类（`xmu.net/db/fs/auth/app`，标识符 `lcNet/lcDb/lcFs/lcAuth/lcApp`），客户端单类（`xmu.cli`，标识符 `lcCli`）；分类文件 `Server/logging.{h,cpp}`、`Client/logging.{h,cpp}`。
- 默认安静：两个 `main.cpp` 在 `QT_LOGGING_RULES` 未设置时 `setFilterRules("xmu.*.debug=false")`；排查时设 `QT_LOGGING_RULES="xmu.*.debug=true"`（或单类 `xmu.db.debug=true`）打开。
- 级别映射：纯执行细节 `qCDebug`；操作失败/安全拒绝（SQL 失败、登录凭据不符、路径越权拒绝、下载打开失败等）`qCWarning`；配置/监听失败保持 `qCCritical`。日志语句一条未删，客户端 8 处绝对路径输出改为文件名或 `<path suppressed>`。
- 约束：`protocol.h` 为 frozen_path 未触碰；不引入第三方日志库。

