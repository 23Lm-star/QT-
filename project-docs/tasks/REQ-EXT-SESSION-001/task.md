# REQ-EXT-SESSION-001 唯一登录 — 任务记录（goal 节点，2026-09-30）

## 档位
- **LARGE**（goal 节点定档）：触碰鉴权/会话架构（SEC-001 同面同级）；生产数据契约变更（`user_info` 加列 + 新表迁移）；校验点位于**所有**业务请求的公共路径，回归面=全部现有功能；跨会话（goal→design→development→独立测试→审计→集成）。主人 2026-09-30 会话内批准方案 a+开工时序（"按施工顺序开始"）。
- 流程版本：Development OS v1.7。分支 `claude/req-ext-session-001-unique-login`，切自 master@`ccfd5de`；本 goal 会话纯文档，不动生产源码。
- 命名注记：用户面谈时暂名 session-003，落档改为新命名空间首号 SESSION-001（无历史空洞）。

## 并行分支冲突核查（2026-09-30 git diff 实测）
| 分支 | 状态 | 冲突结论 |
|---|---|---|
| REQ-EXT-FILE-001 | **活代码**，独立测试节点，距 merge 仅差审计+三事实行+主人 push（`7eba642`） | 唯一实质冲突源 = **C1**；其余经逐文件比对不重叠：其 `mytcpsocket.cpp` 仅动 :246 上传完成 hunk（与我要动的鉴权入口 :53-83/登录 case 零重叠）；`msghandler.cpp` +373 行全在文件操作函数，不碰 `login()`；两 `protocol.h` 其分支确认零命中，本需求方案 a 亦零命中 |
| REQ-EXT-GROUP-001 | 未切分支（开发门=FILE-001 合并） | 零活代码；60 起 PDU 编号已授权其联合分配表，与本需求无关 |
| REQ-EXT-GROUP-002 | 仅 goal 文档 | 未来其群 PDU/广播施工撞我已合并的鉴权入口 = 正常 trunk-sync，后落地方适配，两边代码不丢 |

- **C1（开发段唯一真冲突）**：FILE-001 新建了全局单一 schema 版本闸门（`main.cpp` 启动 fail-fast + `OperateDB::checkSchemaVersion()` + `kFileAssetSchemaVersion=2026092901`）。本需求的 `login_version` 列/`login_record` 表必须注册进同一版本机制。处置规则（主人批准的施工顺序第 2/3 条）：开发会话启动时查 FILE-001 合并态——**已合并**→ `git merge master` 后把本迁移适配进其版本闸门（常量递增、迁移独立文件），冲突消失；**未合并**→ 照常开发，迁移 SQL 写成独立文件 `Server/migrate_ext_session_001.sql`，**不碰 `main.cpp` 启动钩子与 `checkSchemaVersion` hunk**，版本注册留待 trunk-sync 时一次性适配。禁止预先猜测对方行号施工。
- **C2**：`operatedb.{h,cpp}` 双方均在类顶/类尾追加方法 → 机械文本冲突，trunk-sync 时消解。
- **C3**：踢线断开与 `handleOffLine` 竞态（§4.5）是本需求内部设计必做项；做对后成为 GROUP-002 在线广播语义的前置保障。
- **C4**：行为级正向——唯一登录使"同用户单活连接"成立，简化 GROUP 系列 fan-out/游标假设。

## 现状盘点（基线 master@ccfd5de）
- 登录链路：`MsgHandler::login()`(Server/msghandler.cpp:250，caData 两 32B 槽=用户名+密码，caMsg 载验证码) → `OperateDB::handleLogin()`(Server/operatedb.cpp:242，加盐比对+补盐升级) → 连接层登录成功绑定 `m_actorAuthenticated`+连接内计数 `m_actorSessionVersion`(Server/mytcpsocket.cpp:71-83)。
- 统一鉴权入口：`MyTcpSocket::handlMsg()` :53，白名单 `isPublicRequestType()` :461（注册/登录/验证码/心跳），拒绝回包 `authRejectPdu()` :475——版本校验插此。
- 表：`user_info(id,name,pwd,online,salt)`（project-docs/init_test_data.sql:7），无版本列；无登录记录表；登录报文负载无客户端标识承载位（caData 已占满，扩展位=caMsg 附加区）。
- 连接管理：`MyTcpServer::reseed()`(Server/mytcpserver.cpp:115) 持 `m_tcpsocketlistMutex` 按登录名匹配**首个**连接——同账号多连接并存是现状；踢旧需遍历同名全部连接关闭。
- 迁移先例：`Server/migrate_add_password_salt.sql`（ALTER 追加列）；FILE-001 分支另有 schema 版本闸门模式（见 C1，合并后即项目惯例）。
- 事务先例：`operatedb.cpp` 3 处 `db.transaction()`；GROUP-002 计划的"事务内原子分配序号"与本需求 login_version 原子 +1 同手法（MySQL `LAST_INSERT_ID(login_version+1)` 或条件 UPDATE）。

## 开发任务分解（design 节点细化）
- **T1 数据层**：迁移 SQL（`user_info` 加 `login_version` + 建 `login_record`）；`handleLogin` 成功分支改事务：原子 +1、取回新版本、插登录记录（客户端标识入参，可空）。
- **T2 连接层**：socket 新增成员绑定登录版本（与 `m_actorSessionVersion` 并存或归一，design 定）；鉴权入口白名单后追加版本比对，不一致→拒绝并关闭连接。
- **T3 登录踢旧**：登录成功后遍历同用户名其余连接关闭；同用户多连接在线语义收敛为单活。
- **T4 下线竞态**：`handleOffLine` 前校验断开方版本==库中最新版本，非权威会话不得清在线状态。
- **T5 客户端**：持久化客户端唯一 ID（QSettings）生成并随登录负载附加区发送；连接被切断时的提示与回退行为。
- **T6 证据与回归**：仿 SEC-001/FILE-001 harness 模式：双客户端互踢、版本并发对打（不重复不倒退）、竞态断言（踢旧不误清）、白名单四类不受影响、SEC-001 全量回归、protocol.h 零命中 diff 证明。

## 允许路径
`Server/msghandler.{h,cpp}`（仅登录函数区）、`Server/mytcpsocket.{h,cpp}`、`Server/mytcpserver.{h,cpp}`（连接遍历关闭）、`Server/operatedb.{h,cpp}`、新增 `Server/migrate_ext_session_001.sql`、Client 登录/ID 生成/断连提示、`project-docs/tasks|evidence/REQ-EXT-SESSION-001/`、handoff。
**禁止**：两 `protocol.h`（frozen，方案 a 零命中为硬约束）；FILE-001 未合并前触碰 `main.cpp` 启动钩子/`checkSchemaVersion` hunk；GROUP 表域与一对一消息语义；新外部依赖；改既有验收标准。

## 节点计划与启动条件
goal（本会话，纯文档）→ **design**（新会话：方案 a 细化、版本校验的每请求查询 vs 内存缓存裁决、T2 成员归一决策）→ development（启动时执行 C1 规则：查 FILE-001 合并态→已合并则先 trunk-sync；未合并则按禁触 hunk 清单照常）→ independent test（新会话只读）→ repair → independent audit（新会话、不同代理）→ integration（三事实行）→ merge no-ff。

## 停止条件
- 发现零协议改动无法承载客户端唯一 ID（负载附加区不够用/验证码解析冲突）→ 停在 design，向主人报协议变更决策卡，不自行动 frozen 文件。
- 版本校验每请求查询构成不可接受性能回归且缓存方案无法证明一致 → 同上上报。
- FILE-001 合并后其 schema 机制与本盘点冲突 → 回 goal 重估。
