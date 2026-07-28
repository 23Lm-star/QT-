## 🔄 Agent 任务完成 — 交接指令

### 当前 Agent
**项目架构师：任务已完成**

---

### 下一执行 Agent
**项目工程师**

---

### 对该 Agent 的完整提示词

> **以下提示词可直接复制用于调用下一 Agent：**
>
> ```markdown
> 接收 Agent：项目工程师
> 任务：实现好友关系 JSON 聚合迁移与服务端入站 PDU 双层缓冲防护
>
> ## 上游上下文与已确认架构决策
> 用户已明确批准以下两项重大变更；它们均已由架构师写入唯一事实来源：
> - `BUG-DB-004`（严重）：好友表从双向多行关系改为单用户单行的 JSON 好友 ID 数组。
> - `BUG-SEC-003`（阻塞）：服务端必须抵御恶意 PDU 大包和大量未完成/未处理 PDU 对接收缓存的耗尽攻击。
>
> 请先加载：
> 1. `project-docs/defects/README.md`
> 2. `project-docs/interfaces.md`（重点：1.5 入站 PDU 安全边界）
> 3. `project-docs/decisions/R03-20260727-ARC-架构与安全优化设计.md`
> 4. `project-docs/code-map.md`
>
> ## 任务 A：好友表 JSON 聚合迁移（BUG-DB-004）
>
> ### 目标数据模型
> 将 `friend(user_id, friend_id)` 双向关系行迁移为一用户一行：
> ```sql
> CREATE TABLE friend (
>     user_id INT NOT NULL PRIMARY KEY,
>     friend_list JSON NOT NULL
> );
> ```
> `friend_list` 是好友用户 ID 的 JSON 数组，如 `[1001, 1005]`。
>
> ### 必须修改/核查的代码范围
> - `Server/operatedb.cpp`
> - `Server/operatedb.h`（如果需要补充私有辅助接口）
> - 如有建表 SQL、初始化脚本或数据库说明，也一并更新。
>
> 重点改造方法：
> - `OperateDB::handleOnlineUsers()`
> - `OperateDB::handleAddFriendFin()`
> - `OperateDB::handleFlushFriend()`
> - `OperateDB::handleDeleteFriend()`
>
> ### 实现强制要求
> 1. 提供**可重复执行且可回滚**的迁移脚本/步骤：旧表备份 → 新结构创建 → 旧双向关系按 `user_id` 聚合迁移 → 校验迁移前后关系数量 → 原子切换。未经人工核验，禁止删除旧表或旧数据。
> 2. 使用 MySQL JSON 函数，不允许通过拼接 SQL 字符串写入用户数据；继续使用 `QSqlQuery::prepare()` 和绑定参数。
> 3. 增加好友、删除好友必须在**同一个数据库事务**内同时更新双方的 `friend_list`；任一方失败必须回滚。
> 4. 重复添加同一好友不得在数组中产生重复 ID；删除不存在的好友应当返回可诊断失败，不得破坏另一方数据。
> 5. 刷新好友必须由 JSON 数组关联 `user_info` 返回好友名称；在线用户筛选必须排除当前用户及其 JSON 数组中的好友。
> 6. 开始实现时将 `BUG-DB-004` 更新为`修复中`；完成自检后更新为`待复查`。
>
> ## 任务 B：服务端入站 PDU 双层防护（BUG-SEC-003）
>
> ### 影响范围与边界
> 仅修改**客户端 → 服务端**入站接收路径，重点文件：
> - `Server/protocol.h`
> - `Server/mytcpsocket.cpp`
> - 必要时 `Server/mytcpsocket.h`
>
> 禁止修改以下正常下载逻辑：
> - `MyTcpSocket::DOWNLOAD_CHUNK_SIZE = 65536`
> - `MyTcpSocket::sendNextChunk()`
> - 服务端 → 客户端下载 PDU 的大小
>
> 16 KB 是**单个客户端上传至服务端的 PDU 总长度上限**，不是双向 PDU 上限，也不是文件总大小上限。
>
> ### 必须定义的常量
> 在 `Server/protocol.h` 或受该文件共享的协议常量位置定义：
> ```cpp
> static constexpr uint MAX_INBOUND_PDU_SIZE = 16 * 1024;         // 16384 B，C→S 单 PDU 总长
> static constexpr int MAX_PENDING_INBOUND_BYTES = 256 * 1024;   // 单连接未处理缓存上限
> ```
>
> ### `MyTcpSocket::recvmsg()` 强制逻辑
> 1. 每次 `readAll()` append 至 `m_dpulist` 后，立即检查：
>    ```cpp
>    if (m_dpulist.size() > MAX_PENDING_INBOUND_BYTES) {
>        // 记录 peerAddress、缓存长度和原因；clear 后 disconnectFromHost 并 return
>    }
>    ```
> 2. 当缓冲至少有 `sizeof(PDU)` 字节、准备读取头部时，在读取 `caData` / `caMsg` 或调用 `handlMsg()` 前检查：
>    ```cpp
>    const uint totalLen = pdu->uiTotalLen;
>    const bool valid =
>        totalLen >= sizeof(PDU) &&
>        totalLen <= MAX_INBOUND_PDU_SIZE &&
>        pdu->uiMsgLen == totalLen - sizeof(PDU) &&
>        pdu->uiType > ENUM_MSG_TYPE_MIN &&
>        pdu->uiType < ENUM_MSG_TYPE_MAX;
>    ```
> 3. `valid == false`：必须输出包含对端地址、`uiTotalLen`、`uiMsgLen`、`uiType` 的 `qWarning()`；随后 `m_dpulist.clear(); disconnectFromHost(); return;`。禁止继续循环或基于该长度读取柔性数组。
> 4. 每处理一条完整 PDU，立即从 `m_dpulist` 移除对应字节。确认上传块仍由既有 `ResumableUploadSession` 逐块写入磁盘。
>
> ### 关于 GB 级文件传输的硬性约束
> - **不得**依据单连接累计传输字节数、累计 PDU 数量或完整文件总大小断开上传。
> - 文件可以是 GB 级：它由现有的 4096 B 上传块持续传输；接收缓存上限仅约束“这一时刻还未被解析和处理的内存积压”。
> - 不得将 `MAX_PENDING_INBOUND_BYTES` 错误实现为上传总量限制。
> - 正常高速上传不应被误踢：每个已完整接收的上传 PDU 必须同步处理并删除其缓存条目。
> - 如需增加 `setReadBufferSize()` 作为辅助反压措施，保持在 256 KB 或更小；它不能替代上述应用层校验和硬断开逻辑。
>
> ### 自检与完成条件
> 1. 构建 Server 成功，且无新编译警告/错误。
> 2. 构造并验证以下场景：
>    - `uiTotalLen < sizeof(PDU)`：服务器记录警告并断开；
>    - `uiTotalLen > 16384`：服务器不分配大内存、不调用消息处理器、记录警告并断开；
>    - `uiMsgLen != uiTotalLen - sizeof(PDU)`：断开；
>    - 合法 4096 B 上传数据块：持续上传正常；
>    - 大文件多块上传：证明总传输量不会因 16 KB / 256 KB 限制被错误中断；
>    - JSON 迁移后：新增好友、重复新增、刷新列表、删除好友、在线用户排除和事务失败回滚均验证。
> 3. 在完成自检后，把 `BUG-DB-004` 与 `BUG-SEC-003` 均更新为`待复查`，并写明迁移/验证证据。
> 4. 输出协作总则第 15.3 节规定的结构化变更摘要，并按第九节格式移交 **Bug 查找师**进行二次静态审查。阻塞/严重与数据模型变更未经二次审查不得关闭。
> ```

---

### 当前缺陷状态同步

| 缺陷编号 | 当前状态 | 本次变更说明 |
|---------|---------|-------------|
| BUG-DB-004 | 已确认，待修复 | 用户确认采用单用户单行 JSON 好友数组；已明确迁移、事务和一致性约束。 |
| BUG-SEC-003 | 已确认，待修复 | 用户确认仅保护 C→S 入站；单 PDU ≤16 KB，未处理缓存 ≤256 KB；GB 文件的累计传输量不设本层上限。 |

---

### 交接文件清单

| 文件路径 | 说明 |
|---------|------|
| `project-docs/interfaces.md` | 新增 1.5 节，定义服务端入站 PDU 边界与合法性校验。 |
| `project-docs/non-functional.md` | 增加最大合理 PDU 尺寸和安全风险登记。 |
| `project-docs/defects/README.md` | BUG-DB-004、BUG-SEC-003 已确认并待修复。 |
| `project-docs/code-map.md` | 已更新 JSON 数据模型及两个风险区域。 |
| `project-docs/decisions/R03-20260727-ARC-架构与安全优化设计.md` | 用户确认后的正式架构决策及实施边界。 |

---

### 注意事项
- 这两项是**持久化格式与安全核心逻辑变更**：工程师不得扩大范围至客户端 UI、下载链路或其他无关模块。
- 16 KB 只用于客户端到服务端的单个 PDU 总长度。服务端 64 KB 下载分块保持不变。
- 256 KB 只限制某一时刻尚未处理的 `m_dpulist` 内存积压；绝不是传输配额。