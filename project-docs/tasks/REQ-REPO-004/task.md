# REQ-REPO-004 任务记录 — 仓库卫生：取消跟踪 Client/connect.config

- 需求文档：project-docs/requirements/REQ-REPO-004-repo-sanitize.md
- 分支：chore/repo-sanitize（自 master 4fc567a 切出；需求编写基线 b5d7bb3，事实复核全部仍成立）
- 档位：**SMALL**。依据：不触碰 frozen paths（两 protocol.h）与任何产品源码；单 commit 可整体回退；不 push。
- 验收标准：A1–A7（见需求文档 §二）
- Allowed paths：.gitignore、Client/connect.config.template、Client/connect.config（仅取消跟踪）、project-docs/tasks/REQ-REPO-004/task.md、project-docs/evidence/REQ-REPO-004/

## 分步执行实际记录（§3.1，每步分开执行，未合并命令）

| 步骤 | 命令 | 实际结果 |
|---|---|---|
| 1 备份 | cp → Client/connect.config.bak-20260929 | 28 字节，创建成功 |
| 2 确认 | git ls-files / ls | 均有输出（跟踪中且磁盘存在） |
| 3 模板 | cp connect.config → connect.config.template | 内容 3 行：127.0.0.1 / 5000 / ./filesys |
| 4 取消跟踪 | git rm --cached Client/connect.config（单独执行） | `rm 'Client/connect.config'` |
| **5 双检查** | ls + git ls-files | **ls 有输出（磁盘文件仍在，28 字节）；ls-files 为空** ✅ 未复现分支 1 误删坑 |
| 6 加模板 | git add Client/connect.config.template（单独执行） | 索引显示 R（rename 呈现，仅索引层，工作区无损） |
| 7 .gitignore | 凭据段追加 /Client/connect.config、/Client/connect.config.bak-* | check-ignore 命中 .gitignore:77 与 :78 |

## 决策日志

- 2026-09-29 需求分析：文档全部实测复核属实；跟踪文件数按实测 269 记录（文档 226 为基线期数字）；分支切出点记 4fc567a。全库扫描 `.bak/.local/release/` 无其他误跟踪文件，不触发另开需求。
- 2026-09-29 开发前总结获用户批准。
- 模板内容与真实文件保持一致（无密码），不加占位符，遵循需求 §3.3。
- 备份文件 bak-20260929 保留磁盘、已被新规则忽略，不删除（禁止事项 #8）。

## 验收状态

A1–A4：步骤 5/7 实测通过；A5：build.ps1 见证据；A6/A7：合并前跑 §7 五项 + 三门，输出归档 project-docs/evidence/REQ-REPO-004/。
