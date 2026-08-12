# Harness V2 Delivery Flow

## 角色

| 角色 | 权限 |
| --- | --- |
| 项目所有者 | 批准 Plan、ADR、风险例外和 V2 激活 |
| Integration owner | 审查治理合同、管理 queue worker 和受保护分支 |
| Task owner | 在 owned paths 中开发和提交 payload |
| Reviewer | 审查行为、风险、测试和 immutable submission |
| Queue worker | 构造候选、验证 attestation、CAS 发布 |
| CI runner | 执行受信 Gate，不决定需求或状态 |

Agent 可以提出 Plan、TaskSpec 和 ADR，但不能自行成为项目所有者或通过
写入 `approved_by` 完成授权。

## CLI 轮廓

V2 使用一个入口：

```bash
tool/harness/agent.sh validate
tool/harness/agent.sh list
tool/harness/agent.sh claim XT-101
tool/harness/agent.sh tdd-red XT-101
tool/harness/agent.sh verify XT-101 --phase review
tool/harness/agent.sh submit XT-101 --red-sha SHA
tool/harness/agent.sh queue-build XT-101 --train-id train-001
tool/harness/agent.sh queue-reopen XT-101 QUEUE_REF --reason "CI failed"
tool/harness/agent.sh publish XT-101 QUEUE_REF --evidence evidence.json
tool/harness/agent.sh recover XT-101 QUEUE_REF
tool/harness/agent.sh acceptance-close XT-ACCEPT
tool/harness/agent.sh bootstrap-accept \
  refs/heads/queue/bootstrap/CUTOVER_REF --at UTC_TIMESTAMP
tool/harness/agent.sh bootstrap-publish \
  refs/heads/queue/bootstrap/CUTOVER_REF
```

命令命名可以在实现时调整，但每个行为必须只有一个公开入口，禁止通过
调用内部 `mark-*` 子命令绕过执行。

## 规划

### Draft

项目所有者或 Agent 创建 draft Plan。Validator 检查：

- requirement 和 criterion ID 唯一；
- statement 可观察；
- negative definitions 非空；
- implementation 和 acceptance ownership 闭环；
- task dependency DAG 无环；
- criterion evidence 使用已注册 Gate；
- TaskSpec、Plan 和风险策略一致。

### Approval

Plan approval 是独立的人类动作：

1. 读取 canonical Plan digest；
2. 解析调用者的受信身份；
3. 要求项目所有者确认；
4. 写入 approval block；
5. 创建 immutable `approve/<plan>/<content-digest>` ref；
6. 提交 Plan。

Agent 调用 CLI 时不得通过参数指定任意审批者。自动化测试使用隔离的
fixture identity，不复用生产审批路径。

## Claim

`task claim` 是一个 CAS 事务：

1. 读取 protected branch、Plan 和 TaskSpec；
2. 验证 dependencies 的 acceptance attestation；
3. 读取所有 `active` 和 `queued` state refs；
4. 检查 owned-path 交集；
5. 创建 `ready -> active` state event；
6. CAS 更新 `state/<task>`；
7. 从 accepted base 创建 task branch 和 worktree；
8. 写入本地非权威 worktree 映射。

步骤 6 失败时删除临时 worktree，不保留半 claim 状态。
进程在 state CAS 后、worktree 完成前退出时，`claim-recover` 只在 branch
没有用户 commit 的情况下追加 `claim_rollback`。

## TDD

### 适用模式

| Task 类型 | 模式 | Red 要求 |
| --- | --- | --- |
| feature | `red_green` | 行为 criterion 首先失败 |
| bugfix | `regression` | 稳定复现现有合同违反 |
| refactor | `equivalence` | base characterization 成为冻结 oracle |
| test | `mutation` | sentinel/mutant 被测试捕获 |
| governance | `adversarial` | 非法 fixture 被拒绝 |
| documentation | `not_required` | 使用静态文档 Gate |
| acceptance | `evidence_closure` | 不修改实现和测试 |
| investigation | `bounded_evidence` | 不能关闭产品 criterion |

### Red

`tdd red`：

1. 要求 worktree clean；
2. 验证从 base 到 Red 的每个 commit 只修改 proof paths；
3. 在隔离 worktree 执行 focused Gate；
4. 区分 expected failure 与 infrastructure failure；
5. 记录 Red commit、oracle blobs、Gate policy、命令和输出摘要；
6. 创建 TDD attestation；
7. 将 attestation 写入 immutable TDD ref，不创建产品 lifecycle commit。

以下结果不构成 Red：

- command not found；
- compiler/SDK 缺失；
- timeout 或 crash；
- skipped test；
- unrelated assertion；
- Gate 未注册；
- 生产路径曾经加入后又回滚。

### Green 和 review

review 阶段：

1. 验证 Red attestation 仍绑定当前 Plan、TaskSpec 和 Gate policy；
2. 验证 frozen oracle 未删除、未弱化、未改写；
3. 在 reviewed head 执行 focused Green Gate；
4. 运行风险路由要求的 review Gate DAG；
5. 生成人类 review attestation；
6. 不默认重新执行已经可信 attested 的 base/Red。

如果 proof context 改变，Red attestation 失效，任务回到新的 Red attempt。

## Verification

执行器按以下顺序构造 Gate plan：

```text
TaskSpec explicit gates
  union risk-routing minimum gates
  union changed-path impact gates
  union phase minimum gates
  union criterion evidence gates
  -> expand aggregate nodes
  -> reject unknown/cycle
  -> unique leaf set
  -> cache lookup
  -> resource-aware parallel execution
```

缓存命中仍生成本次 plan entry，记录复用的 attestation digest。缓存不能：

- 将本地结果充当 required remote platform；
- 跨 source tree 复用第一版证据；
- 复用 skip、timeout、failure 或 dirty-tree 结果；
- 用 aggregate Gate 缓存掩盖 leaf 变化；
- 绕过 nightly/release 强制重跑策略。

## Submit

`submit` 在 review 通过后：

1. 再次验证 worktree clean 和 source head；
2. 计算完整 source range 和 aggregate payload patch；
3. 绑定 Plan、TaskSpec、Gate policy、TDD 和 review attestations；
4. 创建 immutable `submit/<task>/<attempt>`；
5. 写入 `active -> queued` state event；
6. 删除或回收开发 worktree；
7. 保留 task branch 和 owned-path reservation。

Payload、计划、风险或测试变化不能修改旧 submission，只能新建 attempt。

## Queue

### 入队选择

Queue coordinator 选择满足以下条件的 submission：

- dependencies 已发布或位于 train 前序；
- owned paths 与 train 中任务不冲突；
- source patch 能在 train head 无冲突应用；
- Plan、TaskSpec 和 attestations 未失效；
- CI 并发和资源预算允许。

### Candidate

每个 candidate commit：

- parent 是前一个 candidate 或当前 protected head；
- payload patch 与 submission 相同；
- TaskSpec 不作为运行态 record 写入 patch；
- commit message 来自 TaskSpec delivery metadata；
- trailers 绑定 task、submission 和 patch digest；
- candidate manifest 存在 queue ref，不写入产品树。

### CI

`merge-queue.yml` 对 candidate：

1. 重新计算 impact 和 Gate DAG；
2. 运行 queue 阶段 required gates；
3. 运行 required platform matrix；
4. 生成 Gate 和 criterion artifacts；
5. 验证无 skip、partial matrix 或 stale artifact；
6. 输出 candidate attestation inputs。

同一 candidate 不再创建 acceptance commit，也不运行第二次完整 CI。

## Publish

Queue worker：

1. 验证 CI repository、workflow blob、run attempt 和 head SHA；
2. 验证 required jobs、artifacts、Gate 和 criterion closure；
3. 创建 acceptance attestation；
4. 读取 protected branch 当前 SHA；
5. 要求其等于 candidate parent；
6. CAS push candidate；
7. 验证远端 protected branch 等于 candidate；
8. 追加 `queued -> done` state event；
9. 释放 owned paths；submission、queue 和 attestation refs 保持可审计。

只有步骤 6 持有全局 publication slot，其他任务可以继续开发和验证。

## 失败处理

| 失败 | 处理 |
| --- | --- |
| 本地 Gate 失败 | 保持 `active`，修复后重跑受影响 Gate |
| review 拒绝 | 保持 `active`，旧 review attestation 作废 |
| submission 后 payload 需变更 | `queued -> active`，创建新 attempt |
| candidate 冲突 | 自动从最新 accepted base 重建；无法应用则回 `active` |
| infrastructure CI 失败 | 有界重试，不修改 source payload |
| product CI 失败 | 归档 candidate，任务回 `active` |
| 前序 train 失败 | 后续 submission 在最新 accepted base 新建 train |
| acceptance 验证失败 | 保持 `queued`，不发布 |
| CAS 失败 | 保持 `queued`，重新构造 candidate |
| 发布后 state 写失败 | recovery 根据 branch 和 attestation 补写 |

每次失败都保留 attempt、candidate ref、日志摘要和原因。恢复流程禁止在
包含失败交付的 lineage 上直接 rebase。

`queue-reopen` 保留原 queue ref，创建对应 archive ref，从 immutable
submission source head 恢复开发 worktree，再执行 `queued -> active`。

## Acceptance Closure

Acceptance owner 不创建 candidate。`acceptance-close` 验证所有
implementation task 的 done state、acceptance ref digest、published SHA
ancestry 和 criterion ID 覆盖，随后写 external closure attestation，并以
`active -> done` 完成验收任务。该操作不改变 protected product branch。

## 撤销与取消

- `active` 任务可由项目所有者取消并释放路径；
- `queued` 任务取消前必须从 train 移除并作废 submission；
- 已发布任务不能改写为未发布，只能创建 revert task；
- archive ref 不因取消删除；
- 依赖它的未发布任务自动变为 blocked reason。
