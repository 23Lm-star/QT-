# REQ-EXT-GROUP-002 可靠群文本消息 — 任务记录（goal 节点归纳，2026-09-29）

## 档位
- **LARGE**（goal 节点定档）：触碰消息协议（两 `protocol.h` 为 frozen_paths，追加 PDU 枚举即命中，须主人协议变更批准）、新建数据库表与事务级并发（群内序号原子分配、幂等提交）、在线广播架构（有界异步队列替代现有同步转发）；跨会话大特性（依赖 SEC-001、GROUP-001 先后落地）。需求文档自声明 LARGE。
- 流程版本：Development OS v1.7。分支：`codex/req-ext-group-002-group-messaging`，切出基线 master@`628e491`（goal 节点，仅承载文档）；会话中 SEC-001 并入 master@`7fc47c1`，本分支按规则 `git merge` 同步（头 `6096e32`），actor 门与加盐登录已在树内。
- 节点计划：goal（本会话：归纳+切分支，不动生产源码）→ 等待主人批准 + 依赖合并 → design（计划文档随分支，含 PDU 编号分配表）→ development → independent test（新会话只读）→ repair → independent audit（新会话、不同代理、只读）→ integration（三事实行）→ merge no-ff。

## 依赖核查（2026-09-29 goal 节点实测）
| 依赖 | 状态 | 影响 |
|---|---|---|
| REQ-EXT-SEC-001 身份与授权上下文 | ✅ **已合并 master@`7fc47c1`**（本 goal 会话期间完成；no-ff，审计链 `c7c4eed` PASS → M-1 `eb0ecc1` → B-1 `0efccb9` ACCEPT，pipeline GREEN/honest-0/PASS，48/48 e2e） | 群入口"校验会话"可直接复用其 actor 门（`isPublicRequestType`/`authRejectPdu`，mytcpsocket.cpp:53）与新表无关，前提已解除 |
| REQ-EXT-GROUP-001 群生命周期与成员关系 | ❌ **未开始**（无分支、无 `group_info/group_member/group_invitation` 表） | 本需求以"已验证的群成员关系"为唯一事实源；成员校验、禁言、`last_read_seq` 载体都不存在，开发硬前置 |
| AUTH-002 / FS-003 / LOG-007 等基线 | ✅ 已合并 master | 登录加盐、路径沙箱、日志分类可直接复用 |

**结论：本分支今天只到 goal 节点为止。开发节点启动条件 = GROUP-001 合并 master 后（SEC-001 已就绪），本分支 `git merge master` 同步，且获得主人批准。**

## 现状盘点（代码事实，并入 SEC-001 后的基线 master@7fc47c1）
- **协议**：两 `protocol.h` 枚举已显式分配至 **59**（LISTDIR_RESPOND），`ENUM_MSG_TYPE_MAX=99999`；PDU = 76B 定头（uiTotalLen/uiMsgLen/uiType/caData[64]/caMsg 柔性数组）；`Msg{totalLen,msgLen,Time[30],msg[]}` 嵌于 caMsg；caData 两个 32B 槽=（自己,对方）。Server 侧已有入站上限 `MAX_INBOUND_PDU_SIZE=16KB`、`MAX_PENDING_INBOUND_BYTES=256KB`——**入站有界，出站转发无界队列**。
- **一对一发送**（`Server/msghandler.cpp chat()` :413）：发送人已由服务端 actor 强制覆盖（SEC-001，随 `7fc47c1` 在树内）；`char msg[120]` 定长拷贝、无内容长度校验的存量面仍在——群入口必须自带长度校验，不回改一对一；`OperateDB::seedchatMsg`(:492) 查收件人 online → 插入 `message_info`（issuccess=在线已投/0=离线待补）→ 在线则 `MyTcpServer::reseed` 同步转发。
- **在线投递**（`Server/mytcpserver.cpp reseed()` :115）：持 `m_tcpsocketlistMutex` 遍历连接列表，按登录名匹配**首个**连接直接 `write()`——单目标、同步、在全局锁下写 socket；慢连接阻塞面正是需求 §4.3 要消除的。群广播需要 fan-out 变体且有界队列。
- **离线补偿现状**（`getchatMsg` :524 + `chatmsg()` :1017）：按 `issuccess=0` 一次取全量并**整批置 1**——无序号、无分页、无成员级游标，"拉取即全部已读"。群消息**不得**复用此模型，必须按需求新建 seq 游标 + 分页 + 未读计数。
- **事务能力**：`OperateDB` 已有 `db.transaction()` 先例 3 处（`operatedb.cpp` :411/:469/:594 附近，好友添加/删除等），QMYSQL + DatabaseConnectionPool（RAII、线程隔离）上事务已被证明可用——T1 的"事务内原子分配序号"有现实基线。
- **表**：无任何群相关表；`message_info(seed_name,rev_name,msg,issuccess,seed_time)` 仅一对一。建表先例 = SQL 迁移文件（`Server/migrate_publish_links.sql`）。
- **客户端**：聊天为单窗口 `Chat`（Index→Friend→m_chat），按好友名开窗；无群列表/群窗口/群消息渲染，需新增；`Client/msghandler.cpp` 按 uiType 分发，群响应需新 handler。

## 开发任务分解（待批准，依赖合并后执行）
- **T1 数据层**：`group_message`（`msg_id` 服务端唯一键、`(group_id, seq)` 唯一、`client_msg_id` 幂等键、sender 服务端推导、文本、类型、创建时间）；成员游标用 GROUP-001 `group_member.last_read_seq`。序号在事务内原子分配（MySQL `UPDATE group_info SET last_seq=LAST_INSERT_ID(last_seq+1)` 或 `SELECT ... FOR UPDATE`，见 D3），禁止读后加一。OperateDB 新增窄接口：幂等插入/按游标分页取/未读计数/读确认单调推进。新迁移 SQL 文件随分支。
- **T2 发送入口**：新群聊 PDU case 进 `handlMsg`，一律经 SEC-001 actor 门；服务端校验成员状态（含已退出/被移除即时失效）、禁言与频率、内容长度上限；忽略客户端 sender 字段。事务提交后先回"已持久化"确认（区分于在线送达），再入广播队列。
- **T3 有界广播**：按在线成员 fan-out；每连接（或每群×每连接）有界异步队列，满则丢弃该连接实时投递并留补拉路径（离线补偿兜底），绝不阻塞 DB 事务与其他成员；连接断开自动摘除。慢连接不持 `m_tcpsocketlistMutex` 同步写。
- **T4 离线补偿与分页**：登录/打开群聊按 `seq > last_seq` 分页拉取；读确认只推进当前成员游标（单调、不可回退、不可越权代推）；未读数 = 游标区间内有效消息数。
- **T5 幂等与并发**：同 `client_msg_id` 重复提交返回原消息确认、不重复入库不重复广播；两发送者并发序号不重复不倒退；DB 重试不产生重复行——全部以 SQL/脚本证据承载。
- **T6 成员事件**：加入/退出/移除作为系统消息走同一事务序号通道（与 GROUP-001 事件的衔接在 design 节点定契约）。
- **T7 证据与回归**：仿 SEC-001 `build/sec001/verify.py` 模式做端到端 harness（实时服务端+控制台日志，绑定候选哈希）：并发对打（seq 连续唯一）、重试幂等、离线重连补齐+未读数、非成员/已退出三断（发送/拉取/确认）全拒、广播队列满与 DB 失败路径显式失败、一对一/离线消息/文件功能回归。

## 允许路径
`Server/msghandler.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`、`Server/mytcpserver.{h,cpp}`、`Server/operatedb.{h,cpp}`、新增 `Server/` 群消息队列文件、群消息迁移 SQL、**两 `protocol.h`（仅追加 59 之后的枚举/结构，frozen_paths——须主人批准，见 D1）**、`Client/msghandler.{h,cpp}`、`Client/chat.*`、`Client/index.*`（群 UI 接入）、`project-docs/tasks/REQ-EXT-GROUP-002/`、`project-docs/evidence/REQ-EXT-GROUP-002/`、handoff。
**禁止**：改既有枚举编号/删字段（协议兼容硬约束）；改 `message_info` 一对一语义；顺手实现富媒体/撤回/编辑/转发/群文件/多设备投递状态（需求 §6 排除）；新外部依赖；`Server/filesys/` 入库；在 SEC-001/GROUP-001 未合并前动生产源码。

## 待主人批准的决策（approval card，业务语言）
- **D1 开发启动时机**：SEC-001 已并入 master（本会话期间），剩余硬前置 = **GROUP-001 完成并入 master** + 主人批准。推荐按此顺序开 development。替代 = 提前并行开发——不推荐，成员表/禁言/游标载体都不存在，独立测试必 BLOCKED。
- **D2 协议编号分配**：推荐 = design 节点出一张覆盖 GROUP-001+GROUP-002 的 PDU 编号分配表（60 起连续段，只追加），主人一次性批准，两分支按表施工，避免 frozen 文件二次改动。
- **D3 序号原子分配实现**：推荐 = MySQL `LAST_INSERT_ID(seq+1)` 技巧（单语句免显式行锁，实现简单）；替代 = `SELECT ... FOR UPDATE` 事务内加一（可移植性好、多一次往返）。以并发对打测试证据裁决。
- **D4 广播队列粒度**：推荐 = 每连接一个有界发送队列（慢成员只拖累自己）；替代 = 全局单队列（实现省，但一人卡顿全员延迟，违背 §4.3）。
- **D5 一对一是否迁移**：本分支**不动**一对一 `message_info` 模型（保持回归面稳定）；群游标体系独立。替代 = 顺带统一消息表——范围外，不做。

## 停止条件（需求 §6 对照）
- 无法证明"先提交后广播"（提交失败不得出现广播；广播失败不得丢已提交消息）→ 独立测试报 **BLOCKED**。
- 序号分配拿不到数据库层并发正确性证据（对打测试或 SQL 事务证明）→ 同上 BLOCKED。
- SEC-001/GROUP-001 合并后出现与本盘点冲突的事实 → 回 goal 重估，不在开发节点即兴扩面。

---

## 决策日志（append-only）

- **2026-10-01 分支接手（takeover）**：owner_agent 由 goal 会话的 claude-code 延续，本会话即 claude-code，无跨代理写权冲突。按 multi-agent takeover 协议执行：运行 pipeline → 刷新交接 → 更新 owner。
- **2026-10-01 开节点前清账**：trunk 存在上一会话遗留的未提交基础设施改动（`pipeline/build.ps1` mingw 链接器插件路径修复 + `Client/Server.pro` Release 输出改 `dist/` + `.gitignore`）。按 branching 规则「先清账后开节点」处理：主人选择「切 SMALL 分支提交并合入 trunk」。结果：`small/build-env-and-dist-deploy` → `3933e3c`（pipeline GREEN）→ no-ff `1af57ac` 合入 master。此改动与 GROUP-002 范围无关，不占用本需求分支。
- **2026-10-01 依赖终态**：`REQ-EXT-GROUP-001` ✅ 已合并 `master@ee75441`（审计 PASS 15/15、主人已接受）；`REQ-EXT-SEC-001` ✅ 在树内。goal 节点的两个硬前置全部解除，D1（开发启动时机）条件满足。
- **2026-10-01 trunk 同步**：本分支 `git merge master` → head `b6b3145`（含 GROUP-001 全部生产代码、SEC-001、build/dist 基础设施）。同步后 pipeline **build GREEN**（`b6b3145`, 2026-10-01 10:04）。分支上仅文档差异的盘点结论不变，无需回 goal。
- **2026-10-01 design 节点完成**：产出 `project-docs/tasks/REQ-EXT-GROUP-002/design.md`（协议编号 88–96 分配表、`group_message`+`group_info.last_seq` 数据模型、`LAST_INSERT_ID` 事务内序号分配与幂等短路、先提交后广播不变式、每连接有界广播队列、游标分页与单调已读、系统消息同序号通道、权限三断、客户端群聊窗口、T7 验证矩阵、停止条件）。**未改任何生产源码**（design 节点边界）。
- **2026-10-01 待批准 D2**：协议编号表将 GROUP-001 预留的 88–95 扩至 **88–96**（未读摘要需独立请求/响应对，95 单槽不足），只追加不重排。两份 `protocol.h` 属 frozen_paths，须主人一次性批准后方可进入 development。审批卡见 design.md §14。
- **2026-10-01 本会话只做设计**：主人指示「开发阶段先不用做，先做好设计阶段」，故 development 不在本会话开启，等 D2 批准后另开会话。
- **2026-10-01 D2 已批准**：主人批准协议编号表 88–96（只追加、不重排 1–87）。两份 `protocol.h` 的 frozen_paths 变更已获授权，可进入 development。
