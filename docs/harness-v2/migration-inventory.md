# Harness V1 Migration Inventory

- 采集时间：2026-08-12
- 采集仓库：`https://github.com/blackfrog638/XnnTransfer.git`
- 采集状态：frozen
- Dirty worktree：0
- 运行中 GitHub workflow：0

## Trusted Base

| 名称 | SHA | 归档 ref |
| --- | --- | --- |
| Protected remote baseline | `5fa1a864e2304aea818aa7fb6d0a193a48fe67aa` | `archive/harness-v1/protected-base` |
| Local final V1 snapshot | `61bbdda590d4009dbcaa378ef827d0a7bf99aeff` | `archive/harness-v1/final-local` |

迁移 trusted product base 选择 protected remote baseline。Local final 仅作
旧 Harness 调试归档，不自动进入 V2 accepted lineage。

## Task Worktrees

| Task | Worktree | HEAD | Dirty | Archive ref |
| --- | --- | --- | --- | --- |
| XT-070 | `/Users/bytedance/XnnTransfer/XnnTransfer-XT-070` | `f33d1b10e606c2b036a8fbc17b7361045ec05371` | no | `archive/harness-v1/task/XT-070` |
| XT-083 | `/Users/bytedance/XnnTransfer/XnnTransfer-XT-083` | `30bcee1aab23cf53bda7171501df380a690dbacf` | no | `archive/harness-v1/task/XT-083` |
| XT-093 | `/Users/bytedance/XnnTransfer/XnnTransfer-XT-093` | `a3146aa92056be22a7a9840b5f33bdb9f32ed4ae` | no | `archive/harness-v1/task/XT-093` |
| XT-097 | `/Users/bytedance/XnnTransfer/XnnTransfer-XT-097` | `7897846d55afe2652d2e676f40ae048bd6c58eeb` | no | `archive/harness-v1/task/XT-097` |

Disposition：全部 deferred。V2 不自动恢复任务状态。产品所有者需要新的
V2 Plan/TaskSpec 才能重新启动对应工作。

## Plan Worktrees

| Plan | Worktree | HEAD | Dirty | Archive ref |
| --- | --- | --- | --- | --- |
| async-delivery | `/private/tmp/XnnTransfer_async_delivery_plan` | `7960cb5f2627e8f2d4293eb03f5f89979927d380` | no | `archive/harness-v1/plan/async-delivery` |
| async-delivery-v3 | `/private/tmp/XnnTransfer_async_delivery_v3` | `ba6b23ff2e74d1e88315b560cf29bdd2e8e293cd` | no | `archive/harness-v1/plan/async-delivery-v3` |
| schema4 fixture v2 | `/private/tmp/XnnTransfer_schema4_fixture_v2` | `54df0bea63f6a3d8aa692b4100928e458c1ac8a6` | no | `archive/harness-v1/plan/schema4-lifecycle-fixture-v2` |
| P1 transport composition | `/Users/bytedance/XnnTransfer/XnnTransfer-DP-P1-TRANSPORT-COMPOSITION` | `833f8b33ec6363237cd79890ef63e0e5a731c4c9` | no | `archive/harness-v1/plan/p1-transport-composition` |

Disposition：全部 superseded by Harness V2 planning 或 deferred product
planning。不得直接合入 protected branch。

## Diagnostic Worktree

| Worktree | HEAD | Dirty | Archive ref |
| --- | --- | --- | --- |
| `/private/tmp/XnnTransfer_XT097_red_diag` | `6c219915bf07e3718bb740411b58c34a30962657` | no | `archive/harness-v1/diagnostic/XT-097-red` |

Disposition：historical diagnostic only。

## Bootstrap Worktree

| Worktree | Branch | HEAD at inventory |
| --- | --- | --- |
| `/Users/bytedance/XnnTransfer/XnnTransfer-HarnessV2` | `bootstrap/harness-v2` | `f56e2b5b48c2786dab178483355f89579153d10a` |

## Existing Failure Archives

以下 V1 refs 已存在并继续保留：

```text
archive/XT-096-missing-evidence-witness-20260812
archive/XT-096-missing-witness-source-20260812
archive/XT-096-stale-plan-topology-20260812
archive/XT-096-stale-real-process-source-20260812
archive/XT-097-combined-red-checkpoint-failure-20260812
```

## Verification

执行：

```text
git status --porcelain=v1
git worktree list --porcelain
git for-each-ref ...
gh run list --status in_progress
gh run list --status queued
git push origin refs/heads/archive/harness-v1/*
git ls-remote --heads origin refs/heads/archive/harness-v1/*
```

结果：

- 所有被归档 worktree clean；
- 无运行中或排队中的 workflow；
- 11 个新增远端归档 ref 均匹配预期 SHA；
- 未移动或删除原 task/plan branch；
- 未修改 protected branch。

## Cleanup Gate

只有满足以下条件才允许删除旧 worktree：

1. V2 migration snapshot 已生成；
2. 每个 archive ref 再次通过远端 SHA 校验；
3. 项目所有者确认 deferred/cancelled disposition；
4. V2 切换候选已通过全部测试；
5. 删除操作不影响 bootstrap worktree。
