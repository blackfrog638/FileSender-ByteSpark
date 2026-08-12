# Harness V2 Bootstrap Worklog

本文件记录 bootstrap 期间的人工授权、重要决策、命令、验证和残余风险。
它是审计日志，不是任务运行状态来源。

## 授权

### 2026-08-12：启动规划

项目所有者要求：

- 按新的 Harness V2 架构开始计划；
- 本次重构使用快速通道；
- 不要求旧 Harness 批准对旧 Harness 的替换；
- 计划和文档必须完整；
- 工作过程必须保留可审计痕迹。

执行边界：

- 当前只进行规划，不实现 V2 runtime；
- 不调用旧 `agent.sh` 生命周期；
- 不修改当前 `harness`；
- 不清理旧 branch/worktree/ref；
- ADR 保持 proposed，等待项目所有者评审。

### 2026-08-12：批准实施

项目所有者在完整规划发布后明确指示：

```text
直接开始实行
```

该指令确认：

- 接受 ADR 0017 的 V2 方向；
- 允许状态和 attestation 移出产品历史；
- 允许一次 candidate CI 取代双 CI；
- 允许 speculative merge train；
- 允许 V2 原子替换 V1；
- 继续使用快速通道，不创建旧 XT/record；
- 迁移 trusted base 使用规划开始时的远端
  `origin/harness@5fa1a864e2304aea818aa7fb6d0a193a48fe67aa`。

ADR 已改为 accepted，实施计划已改为 active。Pilot 具体内容在 WP8 前
选择，不阻塞基础实现。

## 基线

| 项目 | 值 |
| --- | --- |
| 仓库 | `https://github.com/blackfrog638/XnnTransfer.git` |
| 本地 harness | `61bbdda590d4009dbcaa378ef827d0a7bf99aeff` |
| origin/harness | `5fa1a864e2304aea818aa7fb6d0a193a48fe67aa` |
| 规划分支 | `bootstrap/harness-v2` |
| 规划 worktree | `/Users/bytedance/XnnTransfer/XnnTransfer-HarnessV2` |
| 日期 | `2026-08-12` |

注意：规划基线不是迁移 trusted product base。后者必须由项目所有者在
阅读迁移审计后明确选择。

## 初始状态观察

- `tool/harness` 有 47 个 Python/Shell 文件，约 2.2 万行；
- 活跃治理元数据约 2.4 万行；
- durable records 包含 67 个 `done`、20 个 `ready`；
- task list 仍将 XT-097 branch record 解析为 `in_progress`；
- 存在 XT-070、XT-083、XT-093、XT-097 task worktree；
- 存在 async-delivery、schema4 fixture、transport composition plan worktree；
- 存在 detached XT-097 diagnostic worktree；
- local harness 比 origin/harness 多一个提交；
- 代表性完整 CI 的关键路径约 15 分钟；
- XT-082 delivery candidate CI 约 20 分钟，acceptance candidate CI
  约 6 分钟，随后 protected branch push 仍会触发 workflow。

## 决策记录

| ID | 状态 | 决策 |
| --- | --- | --- |
| D-001 | confirmed | 使用独立 bootstrap branch/worktree，不修改当前 harness |
| D-002 | confirmed | 规划阶段绕过旧 XT/schema-v4 生命周期 |
| D-003 | confirmed | 静态契约只保留 Plan、TaskSpec 和 Gate policy |
| D-004 | confirmed | 运行状态迁移到每任务独立远端 state ref |
| D-005 | confirmed | Gate DAG 去重并并行执行 |
| D-006 | confirmed | Red attestation 可信时不在 review 默认重放 |
| D-007 | confirmed | 使用 immutable submission 和 speculative merge train |
| D-008 | confirmed | 一次 candidate CI + acceptance attestation |
| D-009 | confirmed | 删除 record-only acceptance commit 和全局 integrated slot |
| D-010 | confirmed | V2 原子替换 V1，不长期双写 |

`confirmed` 表示项目所有者已授权本次 bootstrap 使用该决策。后续新增
或改变公共治理合同，仍需新的明确批准。

### 2026-08-12：V1 冻结和归档

检查结果：

- 11 个相关 worktree 全部 clean；
- GitHub 没有 queued 或 in-progress workflow；
- `origin/harness` 是 local `harness` 的祖先；
- local `harness` 比远端多一个旧治理提交。

创建并推送：

```text
archive/harness-v1/protected-base
archive/harness-v1/final-local
archive/harness-v1/task/XT-070
archive/harness-v1/task/XT-083
archive/harness-v1/task/XT-093
archive/harness-v1/task/XT-097
archive/harness-v1/plan/async-delivery
archive/harness-v1/plan/async-delivery-v3
archive/harness-v1/plan/schema4-lifecycle-fixture-v2
archive/harness-v1/plan/p1-transport-composition
archive/harness-v1/diagnostic/XT-097-red
```

所有远端 ref 已用 `git ls-remote` 校验精确 SHA。原 branch 和 worktree
仍然保留，未执行 cleanup。

### 2026-08-12：WP1/WP2 契约、State 与 Workspace

实现：

```text
tool/harness/model.py
tool/harness/validate.py
tool/harness/git_ops.py
tool/harness/state.py
tool/harness/workspace.py
tool/harness/tests/test_model.py
tool/harness/tests/test_state_workspace.py
make harness-v2-test
```

实现决策：

- 静态契约使用标准库可解析的严格 JSON；
- 不依赖本机偶然安装的 PyYAML；
- 重复 JSON key、未知字段和未规范化 ID fail closed；
- Plan approval 从仓库配置的项目所有者 Git identity 解析，不接受
  `--by` 自由文本；
- State event 使用 Git object plumbing 写入独立 ref；
- 每任务 ref 使用 append-only parent chain 和 compare-and-swap；
- 远端 state push 使用 `--force-with-lease`，失败时回滚本地 ref；
- Claim 失败会追加 `active -> ready` rollback event，不留下半 claim；
- 只有相关 owned path 或绑定治理输入变化才判定 stale。

验证：

```text
python3 -B -m unittest discover -s tool/harness/tests -p 'test_*.py' -v
python3 -m py_compile <V2 modules and tests>
git diff --check
```

结果：

- 21 项测试全部通过；
- 覆盖 approval spoof、stale digest、task command injection、Gate cycle、
  risk downgrade、criterion closure、state tamper、CAS、remote state、
  claim、dependency、rollback 和 stale path；
- Python compile 和 whitespace 检查通过；
- 未调用旧 Harness runtime；
- 未修改 protected branch。

## 命令日志

### 2026-08-12：现状读取

只读命令：

```text
git status --short --branch
tool/harness/agent.sh list
git worktree list --porcelain
git for-each-ref ...
wc -l tool/harness/*
gh run list ...
gh run view ...
```

结果：

- 确认效率瓶颈来自重复 Gate、双 CI、单 integrated slot 和 tracked state；
- 确认已有 async-delivery draft 仍在 schema-v4 上叠加兼容层；
- 未修改任何旧任务、record、branch 或 worktree。

### 2026-08-12：创建规划工作区

命令：

```text
git worktree add -b bootstrap/harness-v2 \
  /Users/bytedance/XnnTransfer/XnnTransfer-HarnessV2 \
  61bbdda590d4009dbcaa378ef827d0a7bf99aeff
```

结果：成功创建独立 planning worktree。

### 2026-08-12：规划文档

创建：

```text
docs/adr/0017-harness-v2-control-plane.md
docs/harness-v2/README.md
docs/harness-v2/architecture.md
docs/harness-v2/contracts.md
docs/harness-v2/delivery-flow.md
docs/harness-v2/threat-model.md
docs/harness-v2/testing-strategy.md
docs/harness-v2/migration.md
docs/harness-v2/implementation-plan.md
docs/harness-v2/acceptance-checklist.md
docs/harness-v2/worklog.md
```

结果：文档已完成，并进入独立 planning commit。

### 2026-08-12：文档验证

命令：

```text
git diff --cached --check
内部 Markdown 相对链接检查
文档索引与一级标题检查
批准状态和旧生命周期调用审计
```

结果：

- 11 个规划文件、2,495 行新增内容进入 staged diff；
- whitespace/diff 检查通过；
- 内部 Markdown 链接检查通过；
- ADR 保持 `proposed`；
- 实施计划保持 `draft` 和“执行授权未获得”；
- 未发现调用旧生命周期推进本次 bootstrap 的指令；
- `make verify` 未运行：本次只有规划文档，且快速通道明确不使用旧
  Harness 对 V2 规划进行自审批。

### 2026-08-12：规划发布

规划内容提交：

```text
9fa059d95230ea4bd773bef90c36475fca21026c
docs(harness): define v2 bootstrap plan
```

发布命令：

```text
git push -u origin bootstrap/harness-v2
git ls-remote --heads origin refs/heads/bootstrap/harness-v2
```

结果：

- 远端 `bootstrap/harness-v2` 创建成功；
- 首次发布 ref 精确指向规划内容提交 `9fa059d`；
- 未修改或发布到 `harness`；
- 分支名不匹配现有 `main`、`harness`、`ci/**` workflow push 条件，
  因此没有调用旧 Harness 验收。

## 待项目所有者确认

1. Trusted product base SHA；
2. state/attestation 移出产品历史；
3. 一次 candidate CI 替代双 CI；
4. speculative merge train；
5. queue worker 身份与权限；
6. 两个 pilot task；
7. ADR 0017 是否接受。

## 残余风险

- 当前旧 worktree 尚未归档或清理；
- planning branch 基于 local harness，不代表最终迁移基线；
- GitHub 对目标 ref namespace 和 branch protection 的能力尚未 live 验证；
- RFC 8785 canonicalization 依赖尚未选型；
- queue worker credential 和签名方案尚未决定；
- 文档中的 SLO 需要实现后的实际基线确认；
- V1 删除清单需要 dependency search 验证。

## 后续日志规则

每个工作包完成时追加：

- source base/head；
- changed paths；
- contract changes；
- commands 和结果；
- skipped Gate 及原因；
- reviewer；
- residual risk；
- rollback point；
- 项目所有者阶段门决定。

禁止覆盖历史日志条目；更正使用新条目说明。
