# REQ-REPO-006 任务记录 —— Git 历史清理

| 项目 | 值 |
|---|---|
| 需求文档 | project-docs/requirements/REQ-REPO-006-history-purge.md |
| 分支 | chore/history-purge（仅承载本记录；重写为全仓库操作，见需求 §7） |
| 等级 | SMALL + 不可逆操作 → 专项批准门 |
| 基线 | 082ebd9（需求文档编写时基线为 b5d7bb3，已漂移，实测事实无变化） |
| 流程版本 | Development OS v1.7 |
| 状态 | ⛔ 开发前总结完成，等待主人批准（Q1–Q4） |
| owner_agent | claude-code |

## 一、需求合理性核查（实测）

需求文档事实全部复核通过：`.git` = 51 MB ✓；总 commit 55；历史 `project-master/` 路径 91 个 ✓；
最大 blob `视频共享系统自动化测试.zip` 23.9 MB ✓；top-8 blob 清单 ✓；git-filter-repo 未安装 ✓
（pip 环境存在，可装）；`Server/connect.config` 历史含 root/123456 ✓（85f58c4 引入，a621309 停止跟踪）；
工作区 `Server/connect.config` / `Client/connect.config` 现均为本地未跟踪文件，重写后仍在磁盘，构建不受影响。

**结论：需求合理、可执行，不改任何源码/配置，只操作 .git 与流程记录。** 但下列前提与细节需修正。

## 二、前提修正（与需求文档不符的事实）

- **C1（最重要）：仓库并非"尚未推送"。** origin = github.com/23Lm-star/QT-，
  origin/master 为本地 master 祖先（本地领先 34）。且 **a621309（取消跟踪）不在 origin/master 内 →
  GitHub 远端 master HEAD 当前仍携带 Server/connect.config，内含 root/123456**。
  即：凭据已在线上 HEAD 暴露，"首次推送前是最后时机"的立论失效。
  后果：本地重写 + force-push 只能清理"未来的新克隆"；已被 fork/缓存的副本无法靠重写收回。
  **真正止血 = 确认密码是否在真实环境复用 → 复用则立即改密/仓库转私有（环境操作，本需求禁止，须另行决定）。**
- **C2：Client/connect.config 历史上从未含 123456**（实测 0 命中）→ Q1 范围无需包含 Client，凭据面只有 Server 一个文件。
- **C3：state.ps1 报的 pending audit（REQ-FS-003 @ f808daf）已过期**：复审 PASS（a396c4d）且分支已全部并入 master，
  无未完结工作阻塞本需求。重写后 f808daf 等旧哈希失效属预期（需求 §9 已述）。
- **C4：§4.6 第 1 项验证有永久假阳性**：`git log --all -p -S"123456" -- Server/` 会命中
  `Server/msghandler.cpp:40` 的字符常量（含子串 `123456789`），永远非空。验证须限定到
  `-S"123456" -- Server/connect.config`，否则重写成功后该项仍"失败"。

## 三、优化建议（O1–O6）

- **O1 一次重写**：三条 filter-repo 合并为单条
  `--path Server/connect.config --path project-master/ --path Server/qtbase/ --invert-paths --force`
  （按 Q1 取子集），减少重写遍数与中间态风险。
- **O2 备份双保险**：`git clone --mirror` 只覆盖已提交对象（51 MB）；工作区 6.2 GB 未跟踪
  build/ 不受重写影响，但顺手把两份本地 connect.config 复制到仓库外，成本 1 行。
- **O3 保留 commit-map**：filter-repo 生成 `.git/filter-repo/commit-map`（旧→新哈希映射），
  重写后立即归档至 project-docs/evidence/REQ-REPO-006/，历史证据（如 f808daf 审计）仍可追溯。
- **O4 前后快照**：重写前把 rev-list 数（55）、.git 体积（51M）、top blob 数入 task 记录，回报要求对照。
- **O5 新增决策点 Q4**：GitHub 仓库可见性（public/private）与 root/123456 是否别处复用——
  决定"改密/转私有"是否先行于本需求。
- **O6 分支语义**：本分支只承载流程记录（docs-only，无源码），批准门关闭、重写执行前不产生其他动作。

## 四、批准门（全部回答前，禁止执行 §七 任何步骤）

| 问题 | 选项 | 文档建议 | 核查后建议 |
|---|---|---|---|
| Q1 清理范围 | A 仅凭据 / B 凭据+project-master / C 再加 qtbase | B | B（C4 修正后凭据面更小；qtbase 大头已在 e700d22 处理，视 top-blob 复查再定 C） |
| Q2 是否重写 | A 全重写 / B 不重写只改密 | A | A，但 **B 的前置（改密）因 C1 已属既成暴露，建议无论选哪个都先回答 Q4** |
| Q3 工具 | A filter-repo / B filter-branch | A | A（不变） |
| Q4（新增） | 仓库是否 public？root/123456 是否复用于真实环境？ | — | 若 public 或复用：先改密/转私有，再执行本需求 |

## 五、执行约束（继承需求文档 §6，全部有效）

无备份不执行；备份置于仓库外并 rev-list 数量相等验证；禁 filter-branch；禁 force-push（主人另批）；
本需求内不改数据库密码；备份保留一个版本周期；gc 前不删备份；不改任何源码/配置。

## 六、下一步

主人回答 Q1–Q4 并批准后 → 新会话按需求 §七 ①–⑩ 执行（开场 state.ps1 确认 clean）。
