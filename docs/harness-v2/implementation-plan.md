# Harness V2 Implementation Plan

## 计划状态

- 状态：active
- 执行授权：项目所有者已于 2026-08-12 明确授权
- 旧 Harness 快速通道：仅规划阶段已授权
- 计划 owner：项目所有者
- 实施协调：integration owner

本文使用 `HV2-WP*` 作为工作包编号，不注册旧式 XT，不创建旧 records，
也不进入旧 backlog。工作包状态记录到 V2 bootstrap worklog，直到 V2
state refs 可用。

## 依赖图

```text
HV2-WP0 Bootstrap baseline
        |
        v
HV2-WP1 Contracts and validators
        |
        +-------------------+
        v                   v
HV2-WP2 State/workspace   HV2-WP3 Gate engine
        |                   |
        |                   v
        |                 HV2-WP4 TDD proof
        |                   |
        +---------+---------+
                  v
            HV2-WP5 Merge train
                  |
                  v
            HV2-WP6 Attested CI
                  |
                  v
            HV2-WP7 Atomic cutover
                  |
                  v
            HV2-WP8 Pilot and acceptance
```

WP2 和 WP3 可以并行。其余工作包必须遵守依赖，不为了赶进度制造临时
provider 或双写状态。

## HV2-WP0：Bootstrap 基线与冻结

目标：建立可审计、可恢复的 V1 冻结面。

交付物：

- trusted base 决策记录；
- 完整 branch/worktree/ref/stash 清单；
- V1 final archive ref；
- 未完成任务 disposition；
- migration snapshot 草案；
- bootstrap branch protection 和 reviewer 列表；
- worklog。

验证：

- 所有 archive ref object 可读取；
- dirty payload 有 bundle 或 archive commit；
- 没有运行中的旧 publish；
- protected branch SHA 与记录一致；
- 未执行破坏性 cleanup。

阶段门：项目所有者明确批准 trusted base。

## HV2-WP1：契约与 Validator

目标：建立 V2 唯一静态模型。

Owned paths：

```text
.agents/manifest.yaml
.agents/gates.yaml
.agents/risk-routing.yaml
.agents/plans/**
.agents/tasks/**
.agents/schemas/**
tool/harness/model.py
tool/harness/validate.py
tool/harness/tests/test_model.py
tool/harness/tests/test_validate.py
```

交付物：

- Plan、TaskSpec、Gate、state 和 attestation JSON Schema；
- YAML 安全子集 parser；
- canonical digest；
- Plan approval identity adapter；
- dependency/criterion/owned-path/risk validator；
- 正负 fixtures；
- 旧 Plan 到 V2 Plan 的只读转换器。

验收：

- 未知字段、重复 ID、孤儿 criterion、环依赖、弱化风险失败；
- Agent 不能通过自由文本批准 Plan；
- 同一内容在平台间产生同一 digest；
- 无运行字段进入静态契约。

## HV2-WP2：State 与 Workspace

目标：用每任务 CAS state ref 替换 tracked runtime record。

Owned paths：

```text
tool/harness/git_ops.py
tool/harness/state.py
tool/harness/workspace.py
tool/harness/tests/test_git_ops.py
tool/harness/tests/test_state.py
tool/harness/tests/test_workspace.py
```

交付物：

- append-only state event；
- CAS transition；
- actor identity；
- claim/worktree；
- owned-path conflict；
- stale base；
- crash recovery；
- legacy migration snapshot reader。

验收：

- 并发 claim 只有一个成功；
- active/queued 路径冲突 fail closed；
- state ref 与本地 cache 不一致时以 ref 为准；
- 删除本地 cache 后可完整恢复；
- dirty worktree 不会被自动清理。

## HV2-WP3：Gate DAG 与执行器

目标：一次解析、去重并并行执行全部 required Gate。

Owned paths：

```text
tool/harness/gates.py
tool/harness/executor.py
tool/harness/tests/test_gates.py
tool/harness/tests/test_executor.py
Makefile
```

交付物：

- DAG parser 和 cycle detection；
- risk/impact/phase Gate union；
- stable unique leaf plan；
- argv executor；
- timeout/output bounds；
- resource scheduler；
- full-tree evidence cache；
- Gate attestation；
- timing。

验收：

- `verify + native_test + transfer_test` 不重复 leaf；
- unknown、cycle、task-authored command 被拒绝；
- 独立 lightweight Gate 并行；
- shared build 不并发破坏；
- key 任一字段变化 cache miss；
- failure/skip/timeout 不缓存。

阶段门：现有 ABI、architecture、security Gate 覆盖对照通过。

## HV2-WP4：TDD Proof

目标：保留有效 TDD 因果关系，删除无价值重复重放。

Owned paths：

```text
tool/harness/tdd.py
tool/harness/attestation.py
tool/harness/tests/test_tdd.py
tool/harness/tests/test_attestation.py
```

交付物：

- red_green、regression、equivalence、mutation、adversarial；
- proof path history scan；
- expected/infrastructure failure classifier；
- frozen oracle；
- Red attestation；
- Green/review proof；
- context invalidation。

验收：

- 生产-before-Red 即使回滚也失败；
- skip/timeout/compiler error 不算 Red；
- oracle 变化使 proof 失效；
- documentation/acceptance 不制造虚假 Red；
- trusted Red attestation 不在 review 默认重跑；
- high/critical policy 可强制远端 replay。

## HV2-WP5：Submission 与 Merge Train

目标：释放开发 Agent，并行验证不可变 reviewed payload。

Owned paths：

```text
tool/harness/merge_queue.py
tool/harness/tests/test_queue.py
```

交付物：

- review attestation；
- immutable submission ref；
- active-to-queued transition；
- candidate construction；
- source range 和 patch equivalence；
- train coordinator；
- failure prefix rebuild；
- bounded retry；
- archive refs。

验收：

- submission 不能重写；
- post-review payload 变化必须新 attempt；
- A/B/C candidate parent 正确；
- A 失败时 B/C 不能发布；
- 后续 payload 从 accepted base 重建；
- queue 阶段开发 worktree 可释放但路径仍保留。

## HV2-WP6：Exact CI 与 Attested Publication

目标：一次 candidate CI 完成验收和发布授权。

Owned paths：

```text
.github/workflows/review.yml
.github/workflows/merge-queue.yml
.github/workflows/nightly.yml
tool/harness/attestation.py
tool/harness/merge_queue.py
tool/harness/tests/test_remote_contract.py
```

交付物：

- review/queue/nightly workflow；
- required jobs/matrix/artifact validator；
- workflow identity；
- acceptance attestation；
- protected-branch CAS publisher；
- publish recovery；
- fake GitHub API fixtures；
- test repository live smoke。

验收：

- wrong SHA、workflow、attempt、job、matrix、artifact 全部拒绝；
- candidate 不包含自身 acceptance metadata；
- metadata 不触发第二次完整 CI；
- publish CAS 竞态只有一个成功；
- push 成功/state 失败可恢复；
- worker 无 Plan approval 权限。

阶段门：项目所有者审查 queue worker 权限。

## HV2-WP7：原子切换

目标：从 V1 单次切换到 V2，不保留双 provider。

交付物：

- AGENTS.md、architecture、testing 和操作手册更新；
- V1 legacy migration snapshot；
- V2 CLI 激活；
- V1 写路径和 schema-v4 runtime 删除；
- old workflow 停用；
- pre-cutover archive refs；
- bootstrap acceptance attestation；
- rollback commit 预演。

验收：

- dependency search 无 V1 runtime 引用；
- V1 命令不能写状态；
- V2 完整 suite 和旧关键质量 Gate 通过；
- protected branch 通过 CAS 到精确 candidate；
- 没有 record-only acceptance commit；
- rollback 演练成功。

## HV2-WP8：Pilot 与最终验收

目标：用真实任务证明吞吐和正确性。

Pilot A：

- 低风险文档/治理；
- 验证 fast lane、state ref、submission 和一次 CI。

Pilot B：

- 窄模块行为变更；
- 验证 Red、Green、Gate DAG、queue 和 attestation。

验收：

- 两任务均无手工修改运行状态；
- 无重复 leaf Gate；
- 每个 candidate 最多一次完整 CI；
- P50 指标达到 README 目标或有经批准的偏差；
- state、attestation 和 protected branch 可恢复；
- 安全负例全部通过；
- 项目所有者批准 ADR 0017 为 accepted。

## 提交与留痕策略

规划阶段：

- 一个规划 commit；
- worklog 记录基线、文件、命令和结果；
- 不附加伪造 XT trailer。

实现阶段：

- 每个 WP 至少一个有意义的 Conventional Commit；
- 大 WP 可按独立行为拆分，不使用 `WIP`/`update files`；
- commit body 写明合同变化和验证；
- worklog 记录 reviewer、结果和残余风险；
- WP 阶段门需要项目所有者或 integration owner 明确确认。

## 快速通道约束

允许：

- 绕过旧 claim、checkpoint、review、integrate、accept；
- 在 bootstrap 分支直接构建 V2；
- 使用 HV2-WP 编号组织工作；
- 一次性人工 bootstrap 发布。

不允许：

- 跳过 ADR、code review、测试或 CI；
- 直接修改 protected branch；
- 未归档清理旧 worktree/ref；
- 自行把 `proposed` 改成 `accepted`；
- 在计划未批准前开始 V2 runtime 实现；
- 为了兼容而长期保留 V1/V2 双写。
