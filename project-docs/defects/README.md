# 缺陷记录

> 本目录记录所有已确认、待修复、已修复和已接受风险的缺陷。
> 维护角色：架构师（仲裁）、Bug 查找师（发现）、工程师（修复）、测试师（回归）

---

## 缺陷状态速查

| 状态 | 含义 |
|------|------|
| 待确认 | 现象可疑，需进一步分析 |
| 已确认，待修复 | 明确缺陷，等待分配修复 |
| 修复中 | 工程师正在处理 |
| 待复查 | 修复完成，等待审查/回归 |
| 已确认，可继续测试，修复延后 | 已知问题不阻塞当前测试 |
| 已接受风险 | 架构师或用户接受不修复 |
| 已修复 | 修复经复查/回归通过 |
| 已关闭 | 问题彻底解决或失效 |
| 重新打开 | 修复未通过验证 |

---

## 缺陷清单

### BUG-ARCH-001 — 枚举值 36 冲突

| 字段 | 值 |
|------|-----|
| **编号** | BUG-ARCH-001 |
| **模块** | ARCH — 协议定义 |
| **等级** | **严重** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `protocol.h:50-53` |
| **描述** | 枚举值 `36` 被三个常量共用：`ENUM_MSG_TYPE_SHAREFILE_FINSH_RESPOND`、`ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND`、`ENUM_MSG_TYPE_MOVEFILE_REGUEST`。服务端 `MsgHandler` 中 `MOVEFILE_REQUEST(36)` 无法与 `SHAREFILE_FINSH_RESPOND(36)` 区分。客户端 `handlMsg()` 中 36 同时处理 `SHAREFILE_AGREE_REQUEST` 和 `SHAREFILE_FINSH_RESPOND`，`MOVEFILE` 未在客户端分发。 |
| **影响** | 移动文件功能不可靠；分享完成状态与移动请求冲突 |
| **建议** | 为 `SHAREFILE_FINSH_RESPOND`、`MOVEFILE_REGUEST`、`MOVEFILE_RESPOND` 分配唯一枚举值，同步更新两端 `handlMsg` 分发逻辑 |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **修复说明** | ① 两端 `protocol.h` 注释值已更正（36/37/38/39）；② 拼写已修复（FINSH→FINISH, REGUEST→REQUEST）；③ `LOGIN_RESPOND` 注释从"注册响应4"更正为"登录响应4"；④ `Server/mytcpsocket.cpp` 新增 `MOVEFILE_REQUEST` case；⑤ 所有引用均已同步更新 |
| **审查结论** | Bug 查找师二次审查通过（2026-07-24）。枚举冲突已彻底解除，两端 protocol.h 一致。**新发现** BUG-ARCH-001-R1（一般）：`MOVEFILE_RESPOND`(39) 在 Client/Server 均无实际引用（仅定义），该工作归属 BUG-FILE-002 承接。不影响本缺陷关闭。 |

---

### BUG-ARCH-001-R1a — `userinformation.cpp:15` 枚举值拼写错误，两端 protocol.h 缺失 GETUSERINFORMATION 枚举值

| 字段 | 值 |
|------|-----|
| **编号** | BUG-ARCH-001-R1a |
| **模块** | ARCH — 协议定义 |
| **等级** | **阻塞**（Client 无法编译） |
| **发现者** | 项目架构师（BUG-ARCH-001 修复复查） |
| **发现文件** | `Client/userinformation.cpp:15`, `Client/protocol.h`, `Server/protocol.h` |
| **描述** | `Client/userinformation.cpp:15` 引用 `ENUM_MSG_TYPE_GETUSERINFORMSTION_REQUEST`（拼写错误），该枚举值在两端 `protocol.h` 中均未定义。 |
| **影响** | Client 编译失败 |
| **建议** | ① 两端 `protocol.h` 新增 `GETUSERINFORMATION_REQUEST`(44) 和 `GETUSERINFORMATION_RESPOND`(45)；② `userinformation.cpp:15` 修正拼写 |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **修复说明** | ① `Client/protocol.h` 第 59-60 行新增 `GETUSERINFORMATION_REQUEST`/`RESPOND`；② `Server/protocol.h` 同位置同步新增；③ `Client/userinformation.cpp:15` 拼写修正为 `ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST` |
| **审查结论** | Bug 查找师二次审查通过（2026-07-24）。详见审查报告。 |

### BUG-ARCH-002 — `tcpsocketlist` 跨线程无锁访问

| 字段 | 值 |
|------|-----|
| **编号** | BUG-ARCH-002 |
| **模块** | ARCH — 服务端并发 |
| **等级** | **严重** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `Server/mytcpserver.h:30`, `Server/mytcpserver.cpp:15,30,46` |
| **描述** | `QList<MyTcpSocket*> tcpsocketlist` 在 `incomingConnection()`（主线程）、`removeSocket()`（Socket 线程）、`reseed()` / `seedAddFriend()`（Socket 线程）中跨线程读写，没有任何互斥保护。`QList` 非线程安全，并发修改可能导致崩溃或数据不一致。 |
| **影响** | 高并发下服务端可能崩溃或错误转发消息 |
| **建议** | 为 `tcpsocketlist` 的所有读写增加 `QMutex` 保护，或改用线程安全容器 |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **修复说明** | ① `mytcpserver.h` 新增 `QMutex m_tcpsocketlistMutex` 成员；② `incomingConnection()`、`removeSocket()`、`seedAddFriend()`、`reseed()` 四个函数均使用 `QMutexLocker` RAII 加锁保护容器访问；③ `removeSocket()` 中 `deleteLater()` 置于锁范围外 |
| **审查结论** | Bug 查找师二次审查通过（2026-07-24）。四个访问点均已正确加锁，`deleteLater()` 正确置于锁外，无死锁/封装/回归问题。 |

---

### BUG-ARCH-003 — 用户信息查询功能 Server 端缺失

| 字段 | 值 |
|------|-----|
| **编号** | BUG-ARCH-003 |
| **模块** | ARCH — 功能完整性 |
| **等级** | **一般** |
| **发现者** | Bug 查找师（BUG-ARCH-001 修复复查） |
| **发现文件** | `Client/userinformation.cpp:15`, `Server/mytcpsocket.cpp`, `Server/msghandler.h` |
| **描述** | Client 端 `userinformation.cpp` 发送 `GETUSERINFORMATION_REQUEST` 查询用户信息，但 Server 端无对应处理函数、无 `MsgHandler` 声明、无 `mytcpsocket` 分发 case。Client 端 `handlMsg()` 也无 `GETUSERINFORMATION_RESPOND` 响应处理。仅有协议枚举值定义不足以使功能可用。 |
| **影响** | 用户信息查询功能不可用，Client 发送请求后无响应 |
| **建议** | ① Server `MsgHandler` 新增 `getuserinformation()` 处理函数（查库返回用户详情）；② `mytcpsocket.cpp` 新增 case；③ Client `handlMsg()` 新增响应分发 |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 本轮不扩展数据库表结构。先做最小闭环：Server/Client 补齐 `GETUSERINFORMATION_REQUEST/RESPOND` 分发与处理，仅返回现有 `user_info` 可提供的用户名、在线状态和查询结果码；昵称、年龄、性别、地址、职业、学校、签名等资料字段显示“未设置/暂无资料”。扩展用户资料表结构需作为独立需求另行评审。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |
| **审查结论** | Bug 查找师二次审查通过（2026-07-24）。全链路已闭合：Server `mytcpsocket.cpp` case + `msghandler` `getuserinformation()` + `operatedb` `handleGetUserInfo()`（预编译查询）+ Client `handlMsg()` case + Client `handlUserInformation()`。遵循架构师最小闭环裁决——仅返回用户名、在线状态和结果码。 |

---

### BUG-DB-001 — SQL 注入风险（全部使用 `QString::arg` 拼接）

| 字段 | 值 |
|------|-----|
| **编号** | BUG-DB-001 |
| **模块** | DB — 数据库访问 |
| **等级** | **严重** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `Server/operatedb.cpp:31,40,58,69,87,105,123,149,170,188,213,239,245,266,278` |
| **描述** | 所有 SQL 操作均使用 `QString::arg()` 进行字符串拼接，未使用预编译语句或绑定参数。恶意用户名可注入任意 SQL。 |
| **影响** | 数据库数据泄露、被篡改或删除 |
| **建议** | 全部迁移到 `QSqlQuery::prepare()` + `bindValue()` 预编译方式 |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 选择一次性全改：`Server/operatedb.cpp` 中所有用户可控 SQL 同轮迁移为 `prepare()` + `bindValue()`；不得分批留下可利用窗口；不得在同一任务中混入密码哈希、表结构或业务规则变更。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |
| **审查结论** | Bug 查找师二次审查通过（2026-07-24）。全部 15+ 处 SQL 操作已迁移到 `prepare()` + `addBindValue()`（`?` 位置占位符），零 `QString::arg()` SQL 拼接残留。硬编码密码在 `operatedb.cpp` 中零残留。空指针检查保留。 |

---

### BUG-DB-002 — 数据库密码硬编码

| 字段 | 值 |
|------|-----|
| **编号** | BUG-DB-002 |
| **模块** | DB — 配置 |
| **等级** | **严重** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `Server/operatedb.cpp:285` |
| **描述** | 数据库连接密码 `"123456"` 硬编码在源码中，无法在部署时更改，且源码泄露即导致数据库凭据泄露。 |
| **影响** | 安全风险；部署到生产环境需修改源码 |
| **建议** | 将数据库配置（主机/端口/用户/密码/库名）外部化到 `connect.config` 或环境变量 |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **修复说明** | ① `connect.config` 扩展为 8 行（IP、端口、文件路径、DB主机、DB端口、DB名、DB用户、DB密码）；② `server.h` 新增 DB 配置成员；③ `server.cpp` `loadconfig()` 解析 DB 配置并提供向后兼容默认值；④ `operatedb.cpp` `connect()` 从 `Server` 单例读取配置 |
| **复查结论** | Bug 查找师二次审查通过（2026-07-24）。`exit(1)` 方案 A 已实施（配置不完整时拒绝启动），DB 密码不再硬编码于源码。`operatedb.cpp:connect()` 正确通过 `Server::getinstance()` 读取配置。遗留项（配置文件嵌入 QRC、缺少注释）此前已记录，不阻塞关闭。 |

---

### BUG-DB-003 — 无数据库断线重连机制

| 字段 | 值 |
|------|-----|
| **编号** | BUG-DB-003 |
| **模块** | DB — 连接池 |
| **等级** | **一般** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `Server/databaseconnectionpool.cpp:126-128` |
| **描述** | 连接池 `acquire()` 中重开失败连接的尝试仅做一次 `db.open()`，无指数退避重试；无心跳保活机制；MySQL 超时断开后连接池不会主动恢复。 |
| **影响** | 数据库临时中断后服务端无法自动恢复 |
| **建议** | 增加连接健康检查与自动重连逻辑 |
| **状态** | **修复中（日期：2026-07-26 架构师分配）** |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 延后处理，原 fix-spec 技术方案需重写。当前 `DatabaseConnectionPool` 不是 `QObject`，不得直接使用 `QTimer(this)`/slots；后续优先设计 `acquire()` 懒重连或由 QObject 持有定时健康检查。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |

---

### BUG-SEC-001 — 密码无盐 MD5

| 字段 | 值 |
|------|-----|
| **编号** | BUG-SEC-001 |
| **模块** | SEC — 认证 |
| **等级** | **一般** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `Server/operatedb.cpp`, `Client/client.cpp:21` |
| **描述** | 客户端使用 `QCryptographicHash::Md5.toHex()` 哈希后传输；服务端存储原始 MD5 值。无盐值，彩虹表可反向。 |
| **影响** | 数据库泄露后密码可被批量破解 |
| **建议** | 使用 bcrypt/scrypt/Argon2 或加盐 SHA-256 |
| **状态** | **修复中（日期：2026-07-26 架构师分配）** |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 延后处理，依赖 BUG-DB-001 完成后再做。未来允许扩展 `user_info.salt`，但必须有独立数据库迁移、旧用户兼容、注册/登录协议说明；不得和 SQL 注入修复混做。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |

---

### BUG-NET-001 — 无应用层心跳机制

| 字段 | 值 |
|------|-----|
| **编号** | BUG-NET-001 |
| **模块** | NET — 网络 |
| **等级** | **一般** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `Server/mytcpsocket.cpp` |
| **描述** | 无应用层心跳包。`QTcpSocket::disconnected` 信号在操作系统检测到断开时才会触发，中间网络设备断开（如 NAT 超时）可能导致假死连接长期占用资源。 |
| **影响** | 资源泄漏；用户状态显示不准确 |
| **建议** | 增加定时心跳 PDU，超时未响应则断开连接并清理资源 |
| **状态** | **修复中（日期：2026-07-26 架构师分配）** |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 延后处理，原 fix-spec 需统一心跳方向后再实施。建议后续采用服务端驱动：Server 定时发 `HEARTBEAT_REQUEST`，Client 回 `HEARTBEAT_RESPOND`，Server 连续 3 次未收到响应则断开清理；不得双端同时主动发 REQUEST。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |

---

### BUG-FILE-001 — 文件下载一次性加载到内存

| 字段 | 值 |
|------|-----|
| **编号** | BUG-FILE-001 |
| **模块** | FILE — 文件下载 |
| **等级** | **一般** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `Server/msghandler.cpp`（`downloadfile()`） |
| **描述** | 服务端下载文件使用 `QFile::readAll()` 一次性读入内存后再封装 PDU 发送。大文件（>100MB）会大量消耗服务端内存。 |
| **影响** | 大文件下载可能耗尽服务端内存 |
| **建议** | 分块读取并分多个 PDU 发送 |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 选择方案 A1：`MyTcpSocket` 状态机 + `QFile` 流式分块。否决方案 B；否决任何仍使用 `readAll()` 或把全文件放入 `QByteArray remainingData` 的实现。需补齐 Client 分块接收与落盘逻辑。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |
| **审查结论** | Bug 查找师二次审查通过（2026-07-24）。服务端 `sendNextChunk()` + `QTimer::singleShot` 实现流式分块发送 `DOWNLOADFILE_DATA` / `FINISH` PDU；Client 端 `handleDownloadData()` / `handleDownloadFinish()` 正确接收；客户端离线时清理下载状态；不再使用 `readAll()` 全量加载。 |

---

### BUG-FILE-002 — MOVEFILE_REQUEST 响应类型错误

| 字段 | 值 |
|------|-----|
| **编号** | BUG-FILE-002 |
| **模块** | FILE — 文件移动 |
| **等级** | **一般** |
| **发现者** | Bug 查找师（BUG-ARCH-001 修复复查） |
| **发现文件** | `Server/mytcpsocket.cpp:124-126`, `Server/msghandler.cpp:403-416` |
| **描述** | `MOVEFILE_REQUEST` 与 `SHAREFILE_AGREE_RESPOND` 共用 `sharefilemove()` 处理函数。但 `sharefilemove()` 返回的响应类型始终是 `SHAREFILE_FINISH_RESPOND`(37)，而非 `MOVEFILE_RESPOND`(39)。Client 端 `handlMsg()` 无 `MOVEFILE_RESPOND` case，客户端永远收不到移动操作的完成通知。此外 `sharefilemove()` 做的是文件拷贝（QFile::copy）而非真正的移动（rename）。 |
| **影响** | 移动文件功能不可靠——服务端完成操作后客户端无感知 |
| **建议** | ① Server 新增专用 `movefile()` 处理函数（执行 rename/move，返回 `MOVEFILE_RESPOND`）；② Client `handlMsg()` 新增 `MOVEFILE_RESPOND` case |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 批准拆分 `movefile()` 并返回 `MOVEFILE_RESPOND`。但 `sharefilemove()` 属于接收分享文件，必须保持复制语义，不得删除发送者源文件；移动文件修复需增加路径合法性校验。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |
| **审查结论** | Bug 查找师二次审查通过（2026-07-24）。独立 `movefile()` 已创建，含路径合法性校验（canonical path）+ rename→copy+remove 回退，返回 `MOVEFILE_RESPOND`(39)。Client 端 `MOVEFILE_RESPOND` case + `handlMoveFile()` 已实现。新发现 BUG-FILE-002-R1（一般）：目标路径校验使用了错误变量（`canonicalSrc` 代替 `canonicalDst`），已单独记录，不阻塞本缺陷关闭。 |

---

### BUG-BUILD-001 — `Server.pro` 中 `config.qrc` 重复引用

| 字段 | 值 |
|------|-----|
| **编号** | BUG-BUILD-001 |
| **模块** | BUILD — 构建 |
| **等级** | **轻微** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `Server/Server.pro:59-62` |
| **描述** | `config.qrc` 在 RESOURCES 中被列出 3 次。Qt qmake 会去重，无编译错误，但属于冗余。 |
| **影响** | 无功能影响 |
| **建议** | 删除重复的 `config.qrc` 条目 |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 批准修复：`Server/Server.pro:59-62` 改为单条 `RESOURCES += config.qrc`。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |
| **审查结论** | Bug 查找师确认（2026-07-24）：`Server.pro:60` 仅一条 `config.qrc` 引用，重复已消除。 |

---

### BUG-BUILD-002 — `.gitignore` 覆盖不充分

| 字段 | 值 |
|------|-----|
| **编号** | BUG-BUILD-002 |
| **模块** | BUILD — 版本控制 |
| **等级** | **轻微** |
| **发现者** | 项目架构师（初版审查） |
| **发现文件** | `.gitignore:1-2` |
| **描述** | `.gitignore` 仅忽略 `build-*/` 和 `Client/_github_bundle_repo/`。`release/` 目录、`Makefile`、`*.o`、`*.user`、`*.qmake.stash`、`ui_*.h`、`qrc_*.cpp`、`moc_*.cpp` 等构建产物未忽略。当前有多个 `??` 未跟踪文件。 |
| **影响** | 构建产物可能被误提交 |
| **建议** | 增加对 `release/`、`*.o`、`Makefile*`、`*.user`、`.qmake.stash`、`ui_*.h`、`moc_*`、`qrc_*.cpp` 的忽略规则 |
| **状态** | **已修复** |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 批准扩展 `.gitignore` 覆盖 qmake/Qt 构建产物；不得忽略源码、`.pro`、`.qrc`、`project-docs/`。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |
| **审查结论** | Bug 查找师确认（2026-07-24）：`.gitignore` 已完整覆盖 Qt/qmake 构建产物（`*.pro.user*`、`Makefile*`、`.qmake.stash`、`debug/`、`release/`、`moc_*`、`ui_*`、`qrc_*`、`*.o`、`*.exe`）+ IDE/OS 文件。 |

---

### BUG-ARCH-001-R1 — `MOVEFILE_RESPOND`(39) 枚举值未被引用

| 字段 | 值 |
|------|-----|
| **编号** | BUG-ARCH-001-R1 |
| **模块** | ARCH — 协议分发一致性 |
| **等级** | **一般** |
| **发现者** | Bug 查找师（BUG-ARCH-001 修复复查） |
| **发现文件** | `Client/client.cpp:110-208`, `Server/msghandler.cpp:403-416` |
| **描述** | `MOVEFILE_RESPOND`(39) 枚举值在 `protocol.h` 已正确定义，但：(1) Server 端 `sharefilemove()` 无论来自 `SHAREFILE_AGREE_RESPOND` 还是 `MOVEFILE_REQUEST`，都返回 `SHAREFILE_FINISH_RESPOND`(37)，不区分返回类型；(2) Client 端 `handlMsg()` 不存在 `MOVEFILE_RESPOND`(39) case。grep 确认 39 仅出现在 protocol.h 定义行。 |
| **影响** | 协议定义与实际分发存在偏差；`MOVEFILE_RESPOND` 目前为死代码 |
| **建议** | 此项归属 BUG-FILE-002 修复范围（新增 movefile() 处理函数 + Client handlMsg 新增 MOVEFILE_RESPOND case） |
| **状态** | **已修复**（由 BUG-FILE-002 承接并完成） |
| **创建时间** | 2026-07-24 |
| **架构师裁决（2026-07-24）** | 继续由 BUG-FILE-002 承接。工程师修复后必须证明 `MOVEFILE_RESPOND(39)` 在 Server 响应和 Client 分发中均有真实引用。详见 `project-docs/decisions/fix-specs-2026-07-24-architect-ruling.md`。 |
| **审查结论** | Bug 查找师确认（2026-07-24）：`MOVEFILE_RESPOND(39)` 现已在 Server `msghandler.cpp:466`（movefile 响应）和 Client `client.cpp:213`（handlMsg 分发）中有真实引用。已随 BUG-FILE-002 一并关闭。 |

---

### BUG-FILE-002-R1 — `movefile()` 目标路径校验使用了错误变量（`canonicalSrc` 代替 `canonicalDst`）

| 字段 | 值 |
|------|-----|
| **编号** | BUG-FILE-002-R1 |
|| **模块** | FILE — 文件移动 |
|| **等级** | **一般** |
|| **发现者** | Bug 查找师（BUG-FILE-002 修复复查，2026-07-24） |
|| **发现文件** | `Server/msghandler.cpp:448` |
|| **描述** | `movefile()` 中第 448 行对目标路径的合法性检查使用了 `canonicalSrc` 而非 `canonicalDst`。 |
|| **影响** | 目标路径校验缺失 |
|| **建议** | 将第 448 行 `canonicalSrc` 改为 `canonicalDst` |
|| **状态** | **修复中（日期：2026-07-26 架构师分配）** |
|| **创建时间** | 2026-07-24 |
|| **修复时间** | 2026-07-24（第一次：机械修正） |
|| **修复人** | 项目工程师 |
|| **修复内容** | 单行变更：`msghandler.cpp:448`，将重复的源路径校验 `canonicalSrc` 改为目标路径校验 `canonicalDst`。 |
|| **审查结论** | ⚠️ 二次审查未通过（2026-07-26）。变量名修正（canonicalSrc→canonicalDst）机械正确，但目标路径校验策略有语义缺陷：`canonicalFilePath()` 对不存在的路径返回空字符串，导致合法移动操作被拦截。**需要补充修复**：改为校验目标父目录的 `canonicalPath()`。详见 `project-docs/bug-reports/R08-20260726-REV-BUG-FILE-002-R1-二次审查.md`。 |

---

### BUG-POOL-001 — PDU 响应发送/释放路径的所有权约定不统一

| 字段 | 值 |
|------|-----|
| **编号** | BUG-POOL-001 |
| **模块** | POOL — 服务端 PDU 生命周期 |
| **等级** | **一般**（由原“严重”下调） |
| **发现者** | Bug 查找师（静态审查） |
| **发现文件** | `Server/mytcpsocket.cpp:139-150,269-270,290-305` |
| **描述** | 当前下载请求在 `handlMsg()` 内直接 `seedMsg()` 并返回 `nullptr`；普通请求由 `recvmsg()` 统一发送。经架构师复核，当前 PDU 没有实际 double-free：下载元数据在分支内仅发送/释放一次，调用方收到 `nullptr` 后不再释放。问题是所有权模式不直观，后续扩展容易误用。 |
| **状态** | **已修复**（2026-07-25 Bug 查找师二次审查通过） |
| **架构师裁决（2026-07-24）** | 保留下载流的分支内发送时序；新增明确命名的“发送并释放”私有辅助方法和所有权注释。普通返回路径继续由 `recvmsg()` 统一发送。不得让同一 PDU 既返回又发送。详见 `project-docs/decisions/static-review-ruling-2026-07-24.md`。 |
| **修复备注** | `Server/mytcpsocket.h` 新增 `sendAndRelease(PDU*)` 私有方法声明及所有权注释块；`Server/mytcpsocket.cpp` 新增 `sendAndRelease()` 实现（nullptr 安全 + write + memfree），下载分支（`DOWNLOADFILE_REQUEST` case）改用 `sendAndRelease()` 替代 `seedMsg()`，`handlMsg()` 前增加所有权规则注释块。`seedMsg()` 保持原有实现不变。 |
| **动态回归结论** | 项目测试师确认（2026-07-25）：静态审查 + 构建验证通过。Client 侧后续深度动态验证被 BUG-TST-CLIENT-001 阻塞，但该崩溃非 POOL 引入，不回退本缺陷状态。 |

---

### BUG-POOL-002 — `Msg::totoalLen` 公共协议字段拼写错误

| 字段 | 值 |
|------|-----|
| **编号** | BUG-POOL-002 |
| **模块** | POOL — 公共协议定义 |
| **等级** | **一般**（由原“严重”下调） |
| **发现者** | Bug 查找师（静态审查） |
| **发现文件** | `Client/protocol.h:80`, `Server/protocol.h:80` |
| **描述** | Client/Server `Msg` 结构均把总长度字段命名为 `totoalLen`；两端当前布局和引用一致，因此不构成运行时协议不兼容，但公共源码接口可维护性差。 |
| **状态** | **已修复**（2026-07-25 Bug 查找师二次审查通过） |
| **架构师裁决（2026-07-24）** | 更名为 `totalLen` 并同步两端全部引用；不得改变字段类型、顺序、布局或枚举值。公共协议修改须 Bug 查找师二次审查。详见 `project-docs/decisions/static-review-ruling-2026-07-24.md`。 |
| **修复备注** | `Client/protocol.h:80`、`Server/protocol.h:80` `struct Msg` 中 `totoalLen` → `totalLen`；`Client/chat.cpp:45,47`、`Client/msghandler.cpp:189`、`Server/msghandler.cpp:590,595` 同步修改引用。`grep -rn \"totoalLen\"` 确认零残留。字段顺序/类型/枚举值未变，wire layout 兼容。⚠️ 须经 Bug 查找师二次审查。 |
| **动态回归结论** | 项目测试师确认（2026-07-25）：两端源码零残留、协议布局兼容；`interfaces.md` 残留旧字段名已由架构师同步修正。 |

---

### BUG-POOL-003 — 两端 PDU 分配器不同的所有权策略说明不足

| 字段 | 值 |
|------|-----|
| **编号** | BUG-POOL-003 |
| **模块** | POOL — PDU 内存管理 |
| **等级** | **轻微**（由原“严重”下调） |
| **发现者** | Bug 查找师（静态审查） |
| **发现文件** | `Client/protocol.cpp`, `Server/protocol.cpp` |
| **描述** | Client 使用 `malloc/free`，Server 使用 `memalloc/memfree`。经架构师复核，PDU 不跨进程转移所有权，接收缓冲区不由 `seedMsg()` 释放，当前配对正确；问题在于契约未被明确记录。 |
| **状态** | **已修复**（2026-07-25 Bug 查找师二次审查通过） |
| **架构师裁决（2026-07-24）** | 不统一分配器，不向 Client 引入 Server 内存池；在协议文档和关键源码补充“同一进程内同策略配对、禁止跨策略释放”的契约说明。详见 `project-docs/decisions/static-review-ruling-2026-07-24.md`。 |
| **修复备注** | 与 POOL-007 合并处理。`Client/protocol.cpp`、`Server/protocol.cpp` 文件顶部新增 PDU 内存管理规则注释块（3 条规则）。`project-docs/interfaces.md` §1.5 补充所有权规则。仅注释/文档变更，无运行时代码改动。 |

---

### BUG-POOL-004 — `handleLogin()` 以 `bool` 隐式转换承载 `int` 返回语义

| 字段 | 值 |
|------|-----|
| **编号** | BUG-POOL-004 |
| **模块** | POOL — 数据库登录 |
| **等级** | **一般**（由原“严重”下调） |
| **发现者** | Bug 查找师（静态审查） |
| **发现文件** | `Server/operatedb.cpp:51-82` |
| **描述** | 登录认证成功后的 online 状态更新直接 `return q.exec()`，依赖 `bool` 到 `int` 的隐式转换。现有结果值恰好正确，但接口语义不明确，且 UPDATE 失败时没有记录 SQL 错误。 |
| **状态** | **已修复**（2026-07-25 Bug 查找师二次审查通过） |
| **架构师裁决（2026-07-24）** | 保留 `1=成功、0=系统/数据库错误、-1=认证失败`，显式返回 `q.exec() ? 1 : 0` 并记录 UPDATE 失败的 `lastError()`；不改协议或业务规则。详见 `project-docs/decisions/static-review-ruling-2026-07-24.md`。 |
| **修复备注** | `Server/operatedb.cpp:78-81` `handleLogin()` 中 `return q.exec()` 改为显式 `if (!q.exec()) { qCritical() << ... << q.lastError(); return 0; } return 1;`。保留 `-1`（用户不存在）和 `0`（系统/DB 错误）语义不变。 |
| **动态回归结论** | 项目测试师确认（2026-07-25）：实际登录成功，Server 返回 `LOGIN_RESPOND(type=4, caData=1)`。 |

---

### BUG-POOL-005 — 空 `ClientTask` 导致无效线程池调度

| 字段 | 值 |
|------|-----|
| **编号** | BUG-POOL-005 |
| **模块** | POOL — 服务端并发架构 |
| **等级** | **一般** |
| **发现者** | Bug 查找师（静态审查） |
| **发现文件** | `Server/clienttask.cpp:10-13`, `Server/mytcpserver.cpp:7,22-24` |
| **描述** | 每个新连接创建并提交 `ClientTask`，但 `run()` 为空；Socket I/O 实际仍在主线程事件循环中执行。 |
| **状态** | **已修复**（2026-07-25 Bug 查找师二次审查通过） |
| **架构师裁决（2026-07-24）** | 移除 `ClientTask`、`QThreadPool` 与提交调用，正式采用单线程 Qt 事件驱动模型；不得在本次修复中把 socket 草率迁移到线程池。详见 `project-docs/decisions/static-review-ruling-2026-07-24.md`。 |
| **修复备注** | 🗑️ 删除 `Server/clienttask.h`、`Server/clienttask.cpp`；`Server/mytcpserver.h` 移除 `#include <QThreadPool>` 和 `QThreadPool m_threadPool` 成员；`Server/mytcpserver.cpp` 移除构造函数中 `setMaxThreadCount(8)` 和 `incomingConnection()` 中 `ClientTask` 创建与 `start()` 调用；`Server/Server.pro` 移除 `clienttask.cpp/.h` 引用。grep 确认源文件无残留。⚠️ 须经 Bug 查找师二次审查。 |
| **动态回归结论** | 项目测试师确认（2026-07-25）：文件删除与构建验证通过，源码零残留。 |

---

### BUG-POOL-006 — 服务端配置失败依赖 GUI 弹框且退出路径不可控

| 字段 | 值 |
|------|-----|
| **编号** | BUG-POOL-006 |
| **模块** | POOL — 服务端启动配置 |
| **等级** | **一般** |
| **发现者** | Bug 查找师（静态审查） |
| **发现文件** | `Server/server.cpp:20-52`, `Server/main.cpp:5-13` |
| **描述** | 配置错误时使用 `QMessageBox`，无头启动会阻塞；构造函数中加载配置并 `exit(1)`，无法以明确的初始化结果向入口报告失败。 |
| **状态** | **已修复**（2026-07-25 Bug 查找师二次审查通过） |
| **架构师裁决（2026-07-24）** | `loadconfig()` 返回 bool，增加显式初始化入口；`main.cpp` 在进入事件循环前处理失败并非零退出；仅使用 `qCritical()`，移除 `QMessageBox` 和 `exit()`。保持 8 行配置格式不变。详见 `project-docs/decisions/static-review-ruling-2026-07-24.md`。 |
| **修复备注** | `Server/server.h` 新增 `bool initialize()` 声明，`loadconfig()` 改为 `bool`；`Server/server.cpp` 构造函数不再加载配置/监听，`loadconfig()` 移除 `QMessageBox`/`exit()` 改用 `qCritical()`+`return false`，新增 `initialize()` 按序执行 `loadconfig()`→校验→`listen()`；`Server/main.cpp` 改为先 `s.initialize()` 失败 `return 1`，成功后 `OperateDB::instance().connect()`。⚠️ 须经 Bug 查找师二次审查。 |
| **动态回归结论** | 项目测试师确认（2026-07-25）：Server.exe 启动正常，监听 `127.0.0.1:5000`，无 QMessageBox 阻塞。 |

---

### BUG-POOL-007 — Client `free()` 与 Server `memfree()` 命名差异

| 字段 | 值 |
|------|-----|
| **编号** | BUG-POOL-007 |
| **模块** | POOL — PDU 内存管理 |
| **等级** | **轻微** |
| **发现者** | Bug 查找师（静态审查） |
| **发现文件** | `Client/client.cpp:226-240` |
| **描述** | Client 的 `free()` 与其 `mkPDU()` 的 `malloc()` 正确配对；与 Server 内存池策略不同。 |
| **状态** | **已修复**（2026-07-25 Bug 查找师二次审查通过） |
| **架构师裁决（2026-07-24）** | 与 BUG-POOL-003 合并处理：不改实现，只补充所有权/配对注释与协议文档。详见 `project-docs/decisions/static-review-ruling-2026-07-24.md`。 |
| **修复备注** | 与 POOL-003 合并处理。`Client/client.cpp` `seedMsg()` 前新增所有权注释（malloc/free 配对说明）；`Server/mytcpsocket.cpp` `seedMsg()` 前新增所有权注释（memalloc/memfree 配对说明）。仅注释变更，无运行时代码改动。 |

---

### BUG-POOL-008 — `Server/msghandler.cpp` 中文注释乱码

| 字段 | 值 |
|------|-----|
| **编号** | BUG-POOL-008 |
| **模块** | POOL — 源码编码与可维护性 |
| **等级** | **轻微** |
| **发现者** | Bug 查找师（静态审查） |
| **发现文件** | `Server/msghandler.cpp:573` 等多处，`Server/Server.pro:5` |
| **描述** | 文件编码为 UTF-8，但多处注释已保存为乱码字节所对应的错误文本；Server qmake 配置也未声明 `utf8_source`。 |
| **状态** | **已修复**（2026-07-25 Bug 查找师二次审查通过） |
| **架构师裁决（2026-07-24）** | 人工改写乱码注释为正确 UTF-8 中文，并在 `Server.pro` 增加 `utf8_source`；不得变更运行时代码逻辑或以不经核验的全文件转码替代。详见 `project-docs/decisions/static-review-ruling-2026-07-24.md`。 |
| **修复备注** | `Server/Server.pro` `CONFIG` 新增 `utf8_source`；`Server/msghandler.cpp` 全部乱码中文注释（约 70 处）逐条人工改写为正确 UTF-8 中文，涵盖注册/登录/查找/好友/聊天/目录/文件/上传/下载等所有函数。未修改任何运行时代码逻辑。grep 确认无残留乱码模式。 |
| **动态回归结论** | 项目测试师确认（2026-07-25）：中文注释 UTF-8 可读，无乱码。 |

---

### BUG-TST-CLIENT-001 — 登录成功后主界面加载文件列表时 Client 崩溃

| 字段 | 值 |
|------|-----|
| **编号** | BUG-TST-CLIENT-001 |
| **模块** | CLIENT — 主界面文件列表加载 |
| **等级** | **严重** |
| **发现者** | 项目测试师（POOL-001~008 动态回归测试） |
| **发现文件** | `Client/msghandler.cpp:138-148`, `Client/friend.cpp:35-39`, `Client/file.cpp:97-135` |
| **描述** | Client 登录成功进入主界面后，列表响应处理链直接更新 `QListWidget` 时崩溃；首次定位于 File，第二次动态回归定位于 `FLUSHFRIEND_RESPOND` → `Friend::flushFriend_LW()`。运行时输出 `pure virtual method called` / `terminate called without an active exception` / exit code 3。 |
| **影响** | 完全阻塞 Client 端全链路动态测试；不影响 Server 侧独立测试；不回退 POOL-001~008 已修复结论。 |
| **状态** | **重新打开**（2026-07-26 R07 架构裁决：先采集原生调用栈，优先验证登录动画 QGraphicsEffect 所有权链；禁止第四轮列表时序猜测性修改） |
| **创建时间** | 2026-07-25 |
| **架构师裁决（2026-07-25）** | 确认为严重缺陷，立即分配工程师修复；不得降级为延后处理。修复后需 Bug 查找师静态二次审查，再由测试师执行 Client 侧动态回归。详见 `project-docs/decisions/R01-20260725-ARC-POOL动态回归裁决.md`。 |
| **修复时间** | 2026-07-25 |
| **修复人** | 项目工程师 |
| **修复内容** | ① 移除 File 构造函数的 flushfile()；② 移除 Friend 构造函数的 flushfriend()；③ handllogin() 中 Index::show() 后通过 QTimer::singleShot(0) 延迟触发初始刷新。详见缺陷详情文档。 |
| **回归结论** | 项目测试师动态回归失败（2026-07-25）。三处修改已正确应用，但崩溃仍存在；26 个文件条目全部 addItem 后，在 Qt 延迟布局事件中触发 `pure virtual method called`。详见 `project-docs/test-reports/R02-20260725-TST-BUG-TST-CLIENT-001-regression.md`。 |
| **架构师重新裁决（2026-07-26）** | 保持严重等级与 **重新打开** 状态；采纳方案 B 增强版。详见 `project-docs/decisions/R05-20260726-ARC-BUG-TST-CLIENT-001回归裁决.md`。 |
| **二次审查结论（2026-07-26）** | ✅ Bug 查找师二次静态审查通过。方案 B 增强版全部 6 项强制检查项通过，无方案 A/C 痕迹，无禁止区域被修改。详见 `project-docs/bug-reports/R05-20260726-REV-BUG-TST-CLIENT-001-二次审查.md`。 |
| **第二次动态回归（2026-07-26）** | ❌ 失败。登录后无 `FLUSHFILE_REQUEST`，但 `FLUSHFRIEND_RESPOND` → `Friend::flushFriend_LW()` 的 `QListWidget::clear()/addItems()` 路径触发 `pure virtual method called`。详见 `project-docs/test-reports/R05-20260726-TST-BUG-TST-CLIENT-001-回归失败.md`。 |
| **架构师第二次裁决（2026-07-26）** | 保持 **严重**、**重新打开**。Friend 与 File 同步使用缓存 + 可见性门控 + 生命周期绑定的合并排队 UI 重建；不抽取公共基类/模板；保留 File 按需刷新。工程师开始时更新为“修复中”，完成后更新为“待复查”。详见 `project-docs/decisions/R05-20260726-ARC-BUG-TST-CLIENT-001-第二次回归裁决.md`。 |
| **三次审查结论（2026-07-26）** | ✅ Bug 查找师三次静态审查通过。全部 8 项强制检查项通过，Friend 与 File 同步实施缓存+合并排队 UI 重建，无 QListWidget 直接写入残留。详见 `project-docs/bug-reports/R06-20260726-REV-BUG-TST-CLIENT-001-三次审查.md`。 |
| **架构师第三次回归裁决（2026-07-26）** | 维持严重、重新打开。R06 日志不能证明 QListWidget 延迟布局是根因；发现 `Client::showLoadingAnimation()` 对 QWidget 已接管的 `QGraphicsOpacityEffect` 在移除后再次 `deleteLater()` 的高危所有权链，且动画结束时序与好友刷新重叠。工程师必须先以 Debug 调试器栈证实，命中后仅定向修复动画链。详见 `project-docs/decisions/R07-20260726-ARC-BUG-TST-CLIENT-001-第三次回归证据化裁决.md`。 |

---

## 缺陷统计

| 等级 | 数量 | 编号 |
|------|------|------|
| 阻塞 | 0 | — |
| **等级** | **数量** | **编号** |
||------|------|------|
|| **严重** | **0** | — |
|| **一般（修复中）** | **4** | DB-003, SEC-001, NET-001, FILE-002-R1 |
|| **轻微** | **0** | — |
|| **合计** | **4** | |

> **未关闭状态分布**：修复中 4 个（DB-003, SEC-001, NET-001, FILE-002-R1）。

> **已修复/关闭**：ARCH-001, ARCH-001-R1a, ARCH-002, ARCH-003, DB-001, DB-002, FILE-001, FILE-002, ARCH-001-R1, BUILD-001, BUILD-002, POOL-001, POOL-002, POOL-003, POOL-004, POOL-005, POOL-006, POOL-007, POOL-008, BUG-TST-CLIENT-001 = **20 个**
