# Harness V2 Bootstrap

## 状态

- 阶段：实施
- 决策状态：ADR 0017 已获项目所有者接受
- 实现状态：WP0 进行中
- 规划分支：`bootstrap/harness-v2`
- 规划基线：`61bbdda590d4009dbcaa378ef827d0a7bf99aeff`
- 快速通道：已授权绕过旧 Harness 的 XT、schema-v4 和
  `claim -> review -> integrate -> accept` 流程

快速通道只豁免旧 Harness 的生命周期，不豁免 Git 留痕、代码评审、
测试、受保护分支、公共契约决策和回滚要求。

## 目标

Harness V2 在保留以下保证的前提下缩短开发和交付时间：

- 一项任务只拥有一个明确的产品或治理边界；
- 并行任务不能修改相交路径；
- 任务提交后 payload 不可被静默替换；
- Gate 命令由仓库策略拥有，任务作者不能注入任意 shell；
- ABI、协议、安全、持久化和架构变更继续使用专项门禁；
- CI 证据绑定精确候选和执行环境；
- 失败尝试保留归档 ref，发布分支只保留已接受历史。

目标指标：

| 指标 | 目标 |
| --- | --- |
| 本地 focused feedback | P50 小于 2 分钟 |
| 提交评审验证 | P50 小于 8 分钟 |
| `queued` 到发布 | P50 小于 20 分钟，P95 小于 35 分钟 |
| 无冲突开发并行度 | 至少 3 个任务 |
| 完整远端 CI | 每个最终候选最多一次 |
| 发布锁持有时间 | 小于 5 秒 |
| 产品分支生命周期提交 | 0 |

## 权威来源

V2 只保留两个版本化事实来源：

1. Delivery Plan：需求、criterion、负例和验收边界。
2. TaskSpec：依赖、owned paths、风险、TDD 模式和交付元数据。

运行时状态、队列状态和验收证明存放在独立远端 refs 中，不写入产品
分支。Dashboard、任务列表和交付报告全部派生，不成为新的事实来源。

## 文档索引

- [ADR 0017](../adr/0017-harness-v2-control-plane.md)：核心架构决策。
- [架构](architecture.md)：目录、组件、边界和数据流。
- [契约](contracts.md)：Plan、TaskSpec、Gate、状态与 attestation。
- [交付流程](delivery-flow.md)：TDD、评审、队列、验收和恢复。
- [威胁模型](threat-model.md)：资产、攻击面和 fail-closed 策略。
- [测试策略](testing-strategy.md)：风险路由、Gate DAG 和 CI 分层。
- [迁移方案](migration.md)：基线、归档、切换和回滚。
- [迁移清单](migration-inventory.md)：V1 refs、worktree 和 disposition。
- [实施计划](implementation-plan.md)：工作包、依赖、交付物和阶段门。
- [验收清单](acceptance-checklist.md)：V2 上线前的机械检查。
- [工作日志](worklog.md)：授权、命令、决策和验证留痕。

## 实施边界

项目所有者已授权直接实施。Bootstrap 分支可以实现 V2 runtime、测试、
workflow 和迁移工具，但仍不得：

- 直接修改当前 `harness` worktree；
- 未归档清理或重写旧任务分支和 worktree；
- 发布新的旧格式 XT、record 或 Delivery Plan；
- 绕过 V2 测试和精确候选 CI 修改 protected branch；
- 在切换验收前把任何 V2 pilot 标记为 `done`。

## 已确认决策

1. 迁移可信产品基线为规划开始时的 `origin/harness@5fa1a86`；
2. 运行状态和 attestation 移出产品历史；
3. 一次候选 CI 取代 acceptance commit 的第二轮 CI；
4. 采用 speculative merge train；
5. V2 原子替换 V1，不长期双写。

Pilot 的具体变更在 WP8 开始前选择，不阻塞基础实现。
