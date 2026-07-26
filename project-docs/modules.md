# 模块划分、职责与依赖

> 生成依据：代码文件、构建文件（`.pro`）、头文件依赖
> 状态：初版

---

## 1. 客户端模块（`Client/`）

### 1.1 模块总览

| 模块 | 文件 | 职责 |
|------|------|------|
| **Client** | `client.{h,cpp}`, `client.ui` | 主窗口：登录/注册界面；TCP 连接管理；PDU 收发；验证码UI协调 |
| **MsgHandler** | `msghandler.{h,cpp}` | 响应分发：根据 `pdu->uiType` 调用各功能模块的处理方法 |
| **Index** | `index.{h,cpp}`, `index.ui` | 主界面（单例）：持有 File/Friend/Chat 子界面；登录成功后的框架 |
| **File** | `file.{h,cpp}`, `file.ui` | 文件管理界面：文件列表展示、上传/下载/删除/重命名/分享操作 |
| **Friend** | `friend.{h,cpp}`, `friend.ui` | 好友管理界面：好友列表、查找用户、添加/删除好友 |
| **Chat** | `chat.{h,cpp}`, `chat.ui` | 聊天界面：消息发送/展示；一对一即时通讯 |
| **OnlineUser** | `onlineuser.{h,cpp}`, `onlineuser.ui` | 在线用户列表界面 |
| **ShareFile** | `sharefile.{h,cpp}`, `sharefile.ui` | 文件分享：选择目标好友的界面 |
| **Uploader** | `uploader.{h,cpp}` | 文件上传工作线程：`moveToThread` 独立线程，分块(4096B)发送 |
| **UserInformation** | `userinformation.{h,cpp}`, `userinformation.ui` | 用户信息展示弹出窗口 |
| **SimpleCaptcha** | `simplecaptcha.{h,cpp}` | 验证码显示组件（本地绘制） |
| **Protocol** | `protocol.{h,cpp}` | PDU 协议定义 + 构造：`malloc/free` 版本 |
| **Config** | `connect.config`, `config.qrc` | 连接配置（IP/端口/路径），嵌入 Qt 资源文件 |

### 1.2 模块间依赖关系

```
Client
 ├── MsgHandler (成员 m_mhptr)
 ├── protocol.h (PDU 结构)
 ├── userinformation.h (弹出窗口)
 └── Index (登录成功后切换)

Index (单例)
 ├── File
 ├── Friend
 ├── Chat
 └── sharefile (通过 File 触发)

Uploader (独立线程)
 ├── protocol.h (构造 PDU)
 └── QTcpSocket (通过信号回主线程发送)
```

---

## 2. 服务端模块（`Server/`）

### 2.1 模块总览

| 模块 | 文件 | 职责 |
|------|------|------|
| **Server** | `server.{h,cpp}` | 服务端入口窗口：加载配置、启动 TCP 监听 |
| **MyTcpServer** | `mytcpserver.{h,cpp}` | TCP 监听器（单例）：管理连接列表、消息转发 |
| **MyTcpSocket** | `mytcpsocket.{h,cpp}` | 单客户端连接：PDU 粘包/半包处理、消息分发 |
| **ClientTask** | `clienttask.{h,cpp}` | 线程池任务包装：`QObject + QRunnable` |
| **MsgHandler** | `msghandler.{h,cpp}` | 请求处理：注册/登录/好友/聊天/文件/验证码 |
| **OperateDB** | `operatedb.{h,cpp}` | 数据库访问（单例）：全部 SQL 操作集中在此 |
| **DatabaseConnectionPool** | `databaseconnectionpool.{h,cpp}` | 数据库连接池（单例）：RAII 管理，按线程隔离 |
| **ResumableUploadSession** | `resumableuploadsession.{h,cpp}` | 断点续传会话管理：写入文件、偏移量连续性校验 |
| **Protocol** | `protocol.{h,cpp}` | PDU 协议定义 + 构造：`memalloc/memfree` 版本 |
| **Config** | `connect.config`, `config.qrc` | 监听配置（IP/端口/文件路径），嵌入 Qt 资源文件 |

### 2.2 自研内存池模块

| 模块 | 文件 | 职责 |
|------|------|------|
| **Common** | `Common.h` | 常量、FreeList、SpanList、SizeClass、Span 定义 |
| **ThreadCache** | `ThreadCache.{h,cpp}` | TLS 无锁线程本地缓存：208 个 FreeList 桶 |
| **CentralCache** | `CentralCache.{h,cpp}` | 中心缓存（单例）：桶级互斥锁，Span 管理 |
| **PageCache** | `PageCache.{h,cpp}` | 页缓存（单例）：全局锁，Span 合并/切分 |
| **ConCurrentAlloc** | `ConCurrentAlloc.{h,cpp}` | 分配入口：根据大小路由到三级缓存或直接 PageCache |
| **RadixTreeHash** | `RadixTreeHash.h` | 基数树页映射（1/2/3 层按平台选择） |
| **ObjectPool** | `ObjectPool.h` | Span 对象池模板 |
| **memalloc** | `memalloc.{h,cpp}` | 分配包装层：MemHeader + 自动降级到 malloc |

### 2.3 模块间依赖关系

```
Server (入口)
 └── MyTcpServer::getinstance().listen()

MyTcpServer (单例)
 ├── MyTcpSocket* (tcpsocketlist)
 ├── ClientTask (线程池包装)
 └── QThreadPool (max 8)

MyTcpSocket (每个连接)
 ├── MsgHandler (m_MsgHandler)
 ├── protocol.h
 ├── OperateDB::instance() (clientOffLine)
 └── MyTcpServer::getinstance() (removeSocket)

MsgHandler
 ├── OperateDB::instance()
 ├── ResumableUploadSession
 ├── protocol.h
 └── Server::getinstance() (文件路径)

OperateDB (单例)
 └── DatabaseConnectionPool::instance()

DatabaseConnectionPool (单例)
 └── QSqlDatabase / QSqlQuery
```

### 2.4 内存池依赖

```
ConCurrentAlloc (入口)
 ├── ThreadCache (TLS, 无锁)
 │    └── CentralCache::FetchRangeObj / ReleaseListToSpans
 ├── CentralCache (单例, 桶级锁)
 │    └── PageCache::NewSpan / ReleaseSpanToPageCache
 └── PageCache (单例, 全局锁)
      ├── RadixTreeHash (地址→Span 映射)
      └── ObjectPool (Span 对象复用)

memalloc (包装层)
 ├── ConCurrentAlloc / ConCurrentFree
 └── malloc / free (降级路径)
```

---

## 3. 配置模块

| 文件 | 归属 | 读取方式 | 内容格式 |
|------|------|----------|----------|
| `connect.config` | Client 和 Server 各一份 | `:/connect.config`（Qt 资源文件） | 3 行：IP / 端口 / 文件根路径 |
| `config.qrc` | Client | 资源文件索引 | 引用 `connect.config` + 图标 |
| `config.qrc` | Server | 资源文件索引 | 引用 `connect.config` |
| `.gitignore` | 仓库根 | Git 过滤 | 忽略 `build-*/` 和 `Client/_github_bundle_repo/` |

> **注意**：`Server/Server.pro` 中 `config.qrc` 被引用了 3 次（`RESOURCES += config.qrc` 重复），此**无编译错误**但属于冗余，应当在重构时清理。

---

## 4. 构建模块

| 构建目标 | 构建文件 | Qt 模块依赖 | 编译标准 |
|----------|----------|-------------|----------|
| Client | `Client/Client.pro` | `core gui network` | C++11, UTF-8 源码 |
| Server | `Server/Server.pro` | `core gui network sql` | C++11 |

**构建命令**：
```bash
cd Client && qmake Client.pro && make
cd Server && qmake Server.pro && make
```

**当前开发环境**：Qt 5.14.2 MinGW 64-bit

