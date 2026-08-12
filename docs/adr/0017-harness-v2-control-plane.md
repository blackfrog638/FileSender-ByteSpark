# ADR 0017: Harness V2 Control Plane

- 状态：proposed
- 日期：2026-08-12
- 请求方：项目所有者
- 决策方：项目所有者、integration owner
- 实施方：Harness V2 bootstrap
- 影响范围：工程治理、任务状态、验证、集成和验收

## 背景

现有 Harness 已经能机械证明任务所有权、review payload、squash patch、
远端 CI SHA 和 criterion evidence，但把规划、运行状态、TDD 取证、
集成与验收写入同一产品 Git 历史。

当前实现包含约 47 个 Python/Shell 文件和约 2.2 万行 Harness 代码，
治理元数据约 2.4 万行。一次普通任务可能在 Red checkpoint、review、
accept、本地 `verify`、criterion evidence、delivery CI 和 acceptance
CI 中重复执行相同 Gate。`integrated` 单槽位还会让互不冲突的任务排队。

schema-v4 首次真实验收暴露了自引用问题：交付提交不能预先包含自身
SHA，但旧 evidence collector 又要求提交中的 record 已经绑定该 SHA。
XT-096 和 XT-097 是由该模型衍生的治理修补，而不是产品交付。

项目所有者已停止正在进行的开发任务，并授权 Harness V2 规划使用快速
通道，不要求旧 Harness 自己批准对自身的替换。

## 决策

### 1. 分离静态契约与运行控制面

产品分支只保存 Delivery Plan、TaskSpec、Gate policy 和产品代码。

运行状态保存到每任务独立的远端 state ref。状态更新使用 compare-and-
swap，不通过修改 `.agents/records/XT-NNN.json` 推进生命周期。

### 2. 收敛版本化事实来源

活跃治理只保留：

- Delivery Plan：需求与 criterion；
- TaskSpec：依赖、所有权、风险和验证合同；
- Gate policy：受信命令和 Gate DAG。

删除 backlog、task markdown、record 和 handoff 之间的字段镜像。
Dashboard 和 handoff 报告由静态契约、state ref 与 attestation 派生。

### 3. 使用 Gate DAG

`verify`、`security` 等聚合 Gate 只描述依赖，不再次执行已完成叶子。
执行器按 Gate ID、输入树、命令、工具链、平台和受控环境生成证据键，
并行执行独立资源组。

任务作者只能选择受信 Gate ID，不能定义命令或声明一个通用 Gate
替代专项风险 Gate。

### 4. TDD 证明按工作类型选择

- bugfix 必须有 regression Red；
- 有行为变化的 feature 必须有 Red-Green criterion test；
- refactor 使用 characterization/equivalence；
- test infrastructure 使用 sentinel 或 mutation；
- 文档、纯规划和 acceptance 不制造虚假 Red。

Red Gate 在 immutable Red commit 上执行一次并产生 attestation。
review 检查 frozen oracle 未变化并执行 Green，不默认重放 base 和 Red。
证明上下文改变时，原 attestation 失效并重新执行。

### 5. 使用 immutable submission 和 merge train

review 通过后生成不可变 submission ref，记录 source range、payload
patch、TaskSpec、Plan、Gate 和 proof 摘要。开发 Agent 随即释放；owned
paths 保留到发布完成。

Queue worker 在最新 accepted base 上构造累积候选：

```text
base -> candidate A -> candidate A+B -> candidate A+B+C
```

候选可以并行验证，受保护分支只通过短 CAS 事务依次推进。前序失败时，
后续 payload 从最新 accepted base 重建，不在失败 lineage 上 rebase。

### 6. 一次候选 CI 与外部 acceptance attestation

最终候选只运行一次所需远端 CI。通过后生成 acceptance attestation，
绑定候选 SHA、任务、payload、Gate、jobs、artifacts、criterion 和
workflow identity。

产品分支直接发布候选 delivery commit，不再创建 record-only acceptance
commit，也不为元数据变化运行第二次完整 CI。

任务 `done` 由以下事实共同派生：

- acceptance attestation 有效；
- 受保护分支包含对应 delivery SHA；
- state ref 存在已发布事件。

### 7. 风险路由取代每次全量验证

PR/review 执行受影响的快速 Gate；queue candidate 执行风险要求的完整
Gate；nightly 和 release 执行全仓库、长 fuzz、性能与完整矩阵。

风险映射由仓库策略拥有，任务作者不能降低。

## 不变约束

- C ABI、wire protocol、安全配置、持久格式和昂贵架构决策仍需 ADR。
- ABI、架构边界、协议向量、安全负例和 no-skip 策略不因提速而弱化。
- review 后 payload 变化必须重新 review。
- 发布使用精确候选 SHA 和受保护 workflow identity。
- 失败尝试必须可审计，但不能污染 accepted branch lineage。

## 影响

正向影响：

- 删除 lifecycle/acceptance 元数据提交；
- 每个候选最多执行一次完整远端 CI；
- 独立任务可以并行开发、评审和候选验证；
- 同一执行上下文内 Gate 去重；
- 产品历史只包含产品或治理交付；
- 不再存在提交必须预知自身 SHA 的模型。

代价：

- state ref 和 attestation 需要独立权限与保留策略；
- queue worker 成为受信发布主体；
- Gate 输入声明错误可能导致不安全缓存；
- speculative train 会消耗额外 CI 并发；
- 从旧记录迁移需要一次性审计。

对应控制：

- 第一版缓存默认绑定完整 source tree，后续才允许经测试的窄输入；
- state ref 只允许 Harness 身份 CAS 更新；
- attestation 使用严格 schema、workflow identity 和 artifact digest；
- queue 并发和 train 深度有上限；
- 迁移保留旧历史和 archive refs，不重写已接受提交。

## 被替代的旧决策

ADR 被接受并激活后：

- ADR 0005 保留 squash payload 和 patch provenance，移除 acceptance commit；
- ADR 0012 保留 owned-path 冲突，运行状态改由 state ref 提供；
- ADR 0014 保留 Delivery Plan 概念，移除 backlog/spec/record 字段镜像；
- ADR 0015 的双 CI acceptance 被一次候选 CI 与 attestation 取代；
- ADR 0016 的 criterion 和 no-skip 意图保留，统一 record schema-v4 和
  默认 base/Red/head 重放被取代。

历史记录继续按当时规则解释，不会被重新标注为 V2。

## 拒绝的替代方案

- 在 schema-v4 上继续增加 queue 兼容层：保留了主要复杂度和双事实源。
- 只优化 Shell 脚本：无法消除 acceptance commit 和全局 integration 槽位。
- 完全依赖 GitHub PR：不能表达本地 worktree 所有权和项目专用 criterion。
- 仅使用数据库保存状态：增加外部服务和灾难恢复边界。
- 将状态继续写在产品分支：必然重新引入 record-only commit 和 CI 自引用。

## 激活条件

本 ADR 只有同时满足以下条件才能改为 `accepted`：

1. 项目所有者批准本 ADR、迁移基线和实施计划；
2. state ref 权限与恢复方案通过测试；
3. Gate DAG 对现有专项 Gate 的覆盖无回退；
4. 一次候选 CI 的 attestation 能阻止伪造、skip 和错误 workflow；
5. 旧 Harness 可通过单次切换被删除，而不是长期双轨；
6. 回滚演练能恢复最后 accepted base 和 state snapshot。
