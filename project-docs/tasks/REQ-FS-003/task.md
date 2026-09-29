# 任务记录：REQ-FS-003 path-traversal-and-ownership

- **tier**: MEDIUM
- **tier 依据**: 触碰生产源码（`Server/msghandler.h/.cpp`）但不触碰 `frozen_paths`（两个 `protocol.h`）、不改对外协议、单分支单主题可回滚；基线 `b5d7bb3`，分支 `fix/path-traversal-and-ownership`
- **owner_agent**: claude-code
- **date**: 2026-09-29
- **状态**: goal 完成（需求分析 + 范围修订 + 分支切出），待 dev 节点

## 需求（业务语言）

服务端多个接收文件路径的入口没有校验"路径是否落在文件沙箱内"：上传可以在任意目录建文件/追加内容，删除和重命名可以先对沙箱外文件动手再补救，另有四处入口（建目录、列目录、切目录、共享复制）完全零校验——其中"共享复制"能把服务器可读的任何文件（含数据库配置）复制进自己的目录再下载。本需求把所有路径入口统一收进一个校验函数。

## 验收标准

见 `project-docs/requirements/REQ-FS-003-path-traversal-and-ownership.md` §二（A1–A12，其中 A10–A12 为 2026-09-29 修订新增）。

## allowed paths

- `Server/msghandler.h`（新增 `isPathInSandbox` 声明）
- `Server/msghandler.cpp`（实现 + 新增拦截 + 去重替换）
- `project-docs/tasks/REQ-FS-003/task.md`
- `project-docs/evidence/REQ-FS-003/`
- `project-docs/requirements/REQ-FS-003-path-traversal-and-ownership.md`（需求修订同步）

## 不做的事（划界）

- 不改协议（`protocol.h` 是 frozen_paths）；不改客户端；不改 `ResumableUploadSession` 接口
- 不引入新的用户级隔离（A 不能删 B）——现有 movefile/listdir 的用户目录根**维持原状**
- 不顺手修其他缺陷（日志分级 REQ-LOG-007、密码加盐 REQ-AUTH-002 各自独立分支）

## 决策日志（append-only）

| 日期 | 决策 | 说明 |
|---|---|---|
| 2026-09-29 | 需求分析完成，档位定为 MEDIUM | 逐行复核基线：缺陷 1（uploadfileinit:743 dirPath 零校验）、缺陷 2（deldir:397-404 / renamefile:495-502 先动手后反推）属实 |
| 2026-09-29 | **盘点更正：路径入口共 10 处，不是 6 处** | 代码核实：`MKDIR`(372)、`changedir`(425)、`flushfile`(461)、`sharefilemove`(566) 同为客户端可控路径且零校验；原文第④步把 MKDIR 误记为"已有校验" |
| 2026-09-29 | **主人拍板①：isPathInSandbox 增加可选根参数**（默认 filesPath） | 原因：movefile/listdir 现有校验的根是用户目录，按原文"照抄替换"会放松成全树隔离，违反文档自身"不收紧不放松"约束；替换时传用户根保持原隔离级别 |
| 2026-09-29 | **主人拍板②：漏盘入口全部纳入本需求**（MKDIR/changedir/flushfile/sharefilemove 做新增拦截，即改动块⑤） | 仍只动 msghandler 两个文件、不碰协议，MEDIUM 可承受；sharefilemove 是读取逃逸链（可复制 connect.config），风险最高 |
| 2026-09-29 | 需求文档已按上述修订更新（§一表扩至 10 入口、§3.1 签名加根参数、§3.2 增至五块、§二增 A10–A12、§七增 3 场景、§九 filesPath 边界坐实） | 实现文档与实际冲突时同步修正 |
| 2026-09-29 | 实测坐实：`connect.config` 第 3 行 filesPath 为相对路径 `./filesys` | `isPathInSandbox` 内对根做 `QDir(root).absolutePath()` 归一为必选项 |
| 2026-09-29 | renamefile 陷阱记录：校验所在目录，不校验目标文件路径 | 新文件名 rename 前不存在，canonical 返回空，照目标路径校验会误拒所有正常重命名（A7 必挂）；现状旧/新名共用同一目录（489-490 行），校验 dirPath 一处即可 |
| 2026-09-29 | 最小必要安全改动集合调整为 ①②③⑤ | ④（去重替换）风险大时可留给后续需求；⑤ 为纯新增拦截，无回归面 |
| 2026-09-29 | 分支 `fix/path-traversal-and-ownership` 从 master `b5d7bb3` 切出，需求目录（11 份文档）随分支首次提交 `73d85d8` 入库 | 关闭 state.ps1 报告的 `?? project-docs/requirements/` 挂起状态 |
