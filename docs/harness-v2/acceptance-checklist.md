# Harness V2 Acceptance Checklist

本清单在实现阶段逐项绑定自动化证据。规划完成不等于以下项目已通过。

## A. 人工授权

- [x] 项目所有者批准 ADR 0017。
- [x] 项目所有者批准 trusted product base。
- [x] 项目所有者批准运行状态移出产品分支。
- [x] 项目所有者批准一次 candidate CI 取代 acceptance commit CI。
- [ ] Integration owner 批准 queue worker 权限。
- [ ] 两个 pilot task 已明确。

## B. 迁移与归档

- [x] 所有 V1 branch、worktree、ref、stash 已盘点。
- [x] Dirty worktree payload 已归档并验证可恢复。
- [x] V1 final archive ref 已创建。
- [x] Legacy accepted task migration snapshot 已生成。
- [x] 未完成任务 disposition 已由项目所有者确认。
- [x] 没有历史任务被伪造为 V2 attestation。
- [x] 没有使用 destructive reset 清理唯一 payload。

## C. 静态契约

- [x] Plan schema 拒绝未知字段、重复 ID 和孤儿 criterion。
- [x] Criterion 必须有 observable statement 和 negative definitions。
- [x] TaskSpec 是依赖、所有权、风险和交付合同的唯一来源。
- [x] TaskSpec 不包含运行状态、owner、SHA 或 CI URL。
- [x] Agent 不能通过自由字符串批准 Plan。
- [x] Plan/Task/Gate canonical digest 跨平台一致。
- [x] Risk routing 不能被 TaskSpec 降低。

## D. State 与 Workspace

- [x] `ready -> active -> queued -> done` 是唯一持久主状态机。
- [x] State event sequence 和 previous digest 完整。
- [x] 所有 ref 更新使用 CAS。
- [x] 并发 claim 只有一个成功。
- [x] Active/queued owned-path 冲突 fail closed。
- [x] Queued task 释放 Agent 但保留路径。
- [x] 本地 cache 删除后状态可从远端恢复。
- [x] Dirty worktree 自动 cleanup 被拒绝。

## E. Gate DAG

- [x] Gate graph 无环且未知节点失败。
- [x] 聚合 Gate 展开为稳定唯一 leaf 集合。
- [x] 同一 leaf 在一次 plan 中最多执行一次。
- [x] TaskSpec 不能定义 command。
- [x] Command 使用 argv，不经 shell 注入。
- [x] Resource group 并行限制生效。
- [x] ABI、协议、安全和持久化专项 Gate 不可被通用 Gate 替代。
- [x] Failure、skip、timeout 和 dirty tree 不缓存。
- [x] Cache key 任一字段变化都 miss。
- [x] Remote platform requirement 不能由 local cache 满足。

## F. TDD

- [x] Feature 行为使用 Red-Green。
- [ ] Bugfix 使用 deterministic regression。
- [ ] Refactor 使用 equivalence。
- [ ] Test infrastructure 使用 mutation/sentinel。
- [ ] Governance 使用 adversarial fixture。
- [x] Documentation 和 acceptance 不制造虚假 Red。
- [x] Base-to-Red 每个 commit 都检查 proof path。
- [x] Production-before-Red 即使回滚也失败。
- [x] Compiler error、timeout、crash 和 skip 不算 Red。
- [x] Frozen oracle 变化使 proof 失效。
- [x] Trusted Red attestation 在未变化时不重复执行。

## G. Submission 与 Queue

- [x] Submission 创建后不可重写。
- [x] Submission 绑定 payload、Plan、TaskSpec、Gate 和 proof digest。
- [x] Post-review payload 变化产生新 attempt。
- [x] Candidate patch 与 submission patch 等价。
- [x] Candidate parent 和 train prefix 精确。
- [x] 非冲突 submission 可并发验证。
- [x] 前序失败使后代 candidate 不可发布。
- [x] 后续 payload 从最新 accepted base 重建。
- [x] 失败 candidate 和 source attempt 有 archive ref。

## H. CI 与 Attestation

- [x] Candidate CI 绑定 repository、workflow blob、run、attempt 和 SHA。
- [x] Required jobs 和 matrix 完整。
- [x] Skipped、neutral、cancelled 和 partial matrix 拒绝。
- [x] Artifact 绑定同一 run 和 source SHA。
- [x] Criterion evidence 覆盖 approved scenarios。
- [x] Candidate 不包含自身 acceptance metadata。
- [ ] 每个最终 candidate 最多一次完整远端 CI。
- [x] Acceptance attestation schema 和 digest 验证通过。
- [ ] Queue worker 无 Plan approval 权限。

## I. 发布与恢复

- [x] Protected branch 发布使用 CAS。
- [ ] Publication lock 持有时间小于 5 秒。
- [x] CAS 竞态只有一个 publisher 成功。
- [x] Attestation 成功但 CAS 失败保持 queued。
- [x] CAS 成功但 state 写失败可自动恢复。
- [x] `done` 同时要求 branch、attestation 和 state event。
- [x] 已发布错误 payload 使用 revert，不重写历史。
- [ ] State inconsistency 使用 append-only revocation/recovery event。

## J. 删除 V1

- [x] V1 runtime write path 已删除。
- [x] `.agents/records` 不再是运行事实来源。
- [x] backlog/spec/record/handoff 字段镜像已删除。
- [x] schema-v4 runtime 和自引用 evidence 路径已删除。
- [x] record-only acceptance commit 已删除。
- [x] 双完整 CI acceptance 已删除。
- [x] `integrated` 全局单槽位已删除。
- [x] V1/V2 不存在长期双 provider。
- [x] Legacy history 仍可从 archive ref 阅读。

## K. 测试与性能

- [x] V2 unit、integration、adversarial 和 recovery tests 通过。
- [ ] macOS、Linux、Windows 路径与 Git 行为通过。
- [x] 旧 ABI、architecture、security Gate 无覆盖回退。
- [ ] Nightly cache-bypass 能发现 impact routing 漏跑。
- [ ] Focused feedback P50 小于 2 分钟。
- [ ] Review P50 小于 8 分钟。
- [ ] Queued-to-publish P50 小于 20 分钟、P95 小于 35 分钟。
- [ ] 至少 3 个无冲突任务可以并行。
- [ ] Pilot A 和 Pilot B 完整交付。

## L. 文档和运维

- [x] AGENTS.md 只描述 V2。
- [x] Architecture 和 testing 文档更新。
- [x] Operator runbook 包含 claim、queue、publish、recover 和 rollback。
- [x] Dashboard 明确是派生只读视图。
- [ ] State/ref/artifact 保留策略已设置。
- [ ] Credential 和 branch protection 审计已完成。
- [x] Bootstrap worklog 包含命令、结果、reviewer 和残余风险。
- [x] ADR 0017 从 proposed 改为 accepted 的提交有项目所有者确认。
