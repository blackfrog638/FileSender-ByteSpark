# Harness V2 Migration

## 原则

- 不重写已接受产品历史；
- 不把旧记录伪装成 V2 原生 attestation；
- 不在旧 Harness 与 V2 之间长期双写；
- 不删除 dirty worktree 或唯一 payload；
- 不在包含失败交付的 lineage 上继续 rebase；
- protected branch 只通过可审计、可回滚的提交前进；
- 每个破坏性动作前生成可验证归档 ref。

## 当前观察基线

规划开始时：

```text
local harness:  61bbdda590d4009dbcaa378ef827d0a7bf99aeff
origin/harness: 5fa1a864e2304aea818aa7fb6d0a193a48fe67aa
planning branch: bootstrap/harness-v2
```

旧 Harness 当前仍能观察到：

- 67 个 durable `done` 记录；
- 20 个 durable `ready` 记录；
- task list 将 XT-097 的 branch record 解析为 `in_progress`；
- XT-070、XT-083、XT-093、XT-097 worktree；
- async-delivery、schema4 fixture 和 transport composition plan worktree；
- detached XT-097 diagnostic worktree；
- local `harness` 比 `origin/harness` 多一个提交。

这些事实只是迁移输入，不代表项目所有者已经接受任何残留任务。

## 阶段 M0：冻结

在实现前：

1. 禁止旧 `agent.sh claim/integrate/accept/cleanup`；
2. 禁止新的 schema-v4 Plan 和 record；
3. 记录所有 worktree、branch、remote ref、stash 和 dirty path；
4. 获取 GitHub 正在运行的 workflow 和 candidate ref；
5. 等待或明确取消远端写操作；
6. 生成冻结报告并由项目所有者确认。

冻结不移动 branch，不删除 worktree，不修改 durable state。

## 阶段 M1：选择可信基线

项目所有者从以下证据选择 V2 产品基线：

- protected remote branch 当前 SHA；
- 最后一个明确接受的产品 delivery；
- 其后的治理提交及授权来源；
- 本地 ahead commit 的内容和 CI；
- 是否包含 disputed schema-v4 bootstrap。

输出：

```text
trusted_product_base=<sha>
v1_archive_head=<sha>
excluded_commits=[...]
decision_owner=<authenticated identity>
decision_time=<UTC>
```

规划阶段不预设具体 SHA。

## 阶段 M2：归档旧运行状态

创建不可变归档 refs：

```text
archive/harness-v1/final
archive/harness-v1/task/XT-070
archive/harness-v1/task/XT-083
archive/harness-v1/task/XT-093
archive/harness-v1/task/XT-097
archive/harness-v1/plan/<name>
archive/harness-v1/diagnostic/<name>
```

每个 worktree：

1. 记录 HEAD、branch、status 和 untracked paths；
2. clean worktree 只需要 archive ref；
3. dirty worktree 创建受限 bundle 或 WIP archive commit；
4. 校验归档 object 可读取；
5. 项目所有者确认后才删除 worktree。

禁止 `git reset --hard`、`git checkout --` 或未归档删除。

## 阶段 M3：历史任务迁移快照

不为旧任务创建伪造的 V2 state event。生成一个迁移快照：

```json
{
  "schema_version": 1,
  "source": "harness-v1",
  "source_head": "<v1-archive-head>",
  "accepted_tasks": [
    {
      "task_id": "XT-001",
      "legacy_record_blob": "<object-id>",
      "legacy_acceptance_sha": "<sha>"
    }
  ],
  "cancelled_or_deferred_tasks": [
    {
      "task_id": "XT-070",
      "legacy_state": "blocked",
      "archive_ref": "..."
    }
  ],
  "created_by": "<authenticated project owner>",
  "created_at": "<UTC>"
}
```

快照存入受保护 migration ref，并由 V2 dependency resolver 只读使用。
旧 `done` 表示 legacy accepted，不声称拥有 V2 Gate attestation。

未完成任务默认进入 `cancelled_or_deferred_tasks`。是否重新建立 V2
TaskSpec 需要项目所有者逐项决定，不能自动恢复。

## 阶段 M4：构建 V2

V2 在 `bootstrap/harness-v2` 上实现：

- 不调用旧生命周期；
- 每个工作包使用普通 Conventional Commit；
- 每个提交更新 bootstrap worklog；
- 每个阶段生成测试和 review 证据；
- 不把 V2 状态写入旧 records；
- 不修改 protected branch。

实现期间旧 Harness 保持冻结，只用于读取历史 fixture。

## 阶段 M5：切换候选

切换提交必须同时：

- 安装 V2 静态契约、CLI、测试和 workflow；
- 迁移当前有效 Plan/TaskSpec；
- 删除 active V1 runtime 入口；
- 删除 record/handoff/backlog 运行依赖；
- 将旧 ADR 标记为历史或被 ADR 0017 取代；
- 保留只读 migration reader；
- 更新 AGENTS.md、architecture 和 testing 文档；
- 不保留可同时写状态的 V1/V2 双 provider。

候选从 `trusted_product_base` 构造，而不是从失败或 disputed lineage
rebase。规划分支上的 V2 payload 通过 patch 迁移到该基线。

## 阶段 M6：Bootstrap 验收

由于旧 Harness 不能批准自身替换，使用一次性人工 bootstrap：

1. 项目所有者确认 ADR 0017 和切换 diff；
2. 独立 reviewer 审查删除项、状态迁移和权限；
3. 推送精确 V2 candidate ref；
4. 使用 `queue/bootstrap/**` 固定 Linux/macOS/Windows matrix，运行完整
   旧质量 Gate 与全部 V2 Gate；
5. 使用 `agent.sh bootstrap-accept <queue-ref> --at <UTC>` 收集 exact run，
   并生成 `attest/bootstrap/<candidate-sha>` acceptance attestation；
6. 验证 candidate 没有未声明 payload；
7. 更新 required context 后使用 `agent.sh bootstrap-publish <queue-ref>`
   CAS fast-forward protected branch，并复核远端 readback；
8. 初始化 V2 migration/state refs；
9. 禁用旧 candidate workflow；
10. 记录远端 branch、attestation 和 ref 结果。

该过程不创建旧 record-only acceptance commit。
Bootstrap artifact 使用独立 schema，不声明不存在的 TaskSpec 或产品
criterion；普通 `queue/**` 仍必须从 approved task contracts 机械推导
matrix 和 evidence。

## 阶段 M7：试运行

选择两个低风险、互不冲突的真实变更：

- 一个文档/治理任务，验证 fast lane；
- 一个窄模块代码任务，验证 TDD、Gate DAG 和 queue。

试运行期间：

- train 深度限制为 1；
- cache 只绑定完整 source tree；
- queue worker 需要人工 publish confirmation；
- 每次状态转换与旧基线耗时对比；
- 任一 invariant 失败立即停止新 claim。

两项成功后：

- train 深度提升到 3；
- 自动发布仅对通过全部 attestation 的 candidate 开启；
- 旧 Harness 从运行文档删除；
- nightly 开始监控 impact routing 漏跑。

## 回滚

### 发布前失败

- 保留 candidate 和失败日志；
- protected branch 不变；
- archive V2 attempt；
- 从 `trusted_product_base` 修复后重建；
- 不在失败 candidate 上继续 rebase。

### 发布后、试运行前失败

- 禁止强推回旧 SHA；
- 创建一个基于当前 protected head 的 revert delivery；
- 恢复 V1 文件和 workflow；
- 使用 bootstrap 全量 CI；
- CAS 发布 revert；
- V2 state/attestation refs保留归档，不删除。

### 试运行后数据不一致

- 停止 queue publisher；
- 从 protected branch、attestation 和 state refs重建状态；
- 如果产品分支正确，只修复控制面；
- 如果错误 payload 已发布，使用独立 revert task；
- 不修改已存在的 acceptance attestation，追加 revocation event。

## 删除清单

切换完成后，预期删除或归档：

```text
.agents/backlog.yaml
.agents/records/
.agents/handoffs/
tool/harness/governance.py
tool/harness/evidence.py
tool/harness/tdd_contract.py
tool/harness/tdd_proof.py
tool/harness/defect_proof.py
tool/harness/github_ci.py
tool/harness/task_conflicts.py
tool/harness/new_task.sh
旧 agent.sh 生命周期实现
旧 dashboard 状态聚合适配
```

实际删除必须由 dependency search 和 architecture test 证明没有生产引用。
历史文件仍可从 `archive/harness-v1/final` 读取。

## 迁移完成定义

- protected branch 只包含一个 V2 切换 delivery；
- V1 写路径不可调用；
- 旧 history 和 worktree payload 全部可从 archive ref 恢复；
- legacy accepted 任务由 migration snapshot 读取；
- V2 任务只使用 Plan、TaskSpec 和 state ref；
- candidate 只执行一次完整 CI；
- rollback 演练完成；
- 两个 pilot task 达到 SLO 且无 invariant 回退。
