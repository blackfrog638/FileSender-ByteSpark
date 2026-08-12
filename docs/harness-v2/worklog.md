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
| D-003 | proposed | 静态契约只保留 Plan、TaskSpec 和 Gate policy |
| D-004 | proposed | 运行状态迁移到每任务独立远端 state ref |
| D-005 | proposed | Gate DAG 去重并并行执行 |
| D-006 | proposed | Red attestation 可信时不在 review 默认重放 |
| D-007 | proposed | 使用 immutable submission 和 speculative merge train |
| D-008 | proposed | 一次 candidate CI + acceptance attestation |
| D-009 | proposed | 删除 record-only acceptance commit 和全局 integrated slot |
| D-010 | proposed | V2 原子替换 V1，不长期双写 |

`confirmed` 只表示本次规划动作已获授权。`proposed` 必须经项目所有者
评审 ADR 0017 后才能进入实现。

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
