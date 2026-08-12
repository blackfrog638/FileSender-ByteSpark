# Harness V2 Contracts

## 通用规则

- 静态契约使用 UTF-8 YAML，禁止 YAML anchors、aliases 和自定义 tags。
- 运行事件与 attestation 使用 UTF-8 JSON。
- 签名和摘要输入使用 RFC 8785 JSON Canonicalization Scheme。
- 内容摘要使用 SHA-256，Git 对象同时记录完整 object ID。
- 时间使用 RFC 3339 UTC；持续时间使用单调时钟纳秒值。
- ID 必须匹配：
  - Plan：`DP-[A-Z0-9-]+`
  - Task：`XT-[0-9]{3,}`
  - Requirement：`REQ-[A-Z0-9-]+`
  - Criterion：`CRIT-[A-Z0-9-]+`
  - Gate：`[a-z][a-z0-9_]*`
- 未知字段 fail closed。
- 静态契约禁止运行时状态、owner、SHA、URL 和证据字段。

实际 JSON Schema 在实现工作包中根据本文件生成，并用正负 fixture 证明
等价；本文件在规划阶段是语义合同。

## Delivery Plan

```yaml
schema_version: 1
id: DP-P1-DELIVERY
title: P1 LAN transfer
status: approved
source:
  kind: roadmap
  path: docs/roadmap.md

requirements:
  - id: REQ-P1-ONE-FILE
    statement: Transfer one accepted file over authenticated transport.
    criteria:
      - id: CRIT-P1-EXACT-BYTES
        statement: Receiver atomically commits the exact accepted bytes.
        negative_definitions:
          - In-memory transport does not qualify.
          - Automatic acceptance does not qualify.
        evidence:
          gates:
            - one_file_e2e
          scenarios:
            - explicit_accept
            - exact_bytes
            - atomic_commit
          topology: two_process_tls
          platforms:
            - linux
            - macos
            - windows
          allow_skipped: false
    implementation_tasks:
      - XT-101
    acceptance_owner: XT-102

approval:
  approved_by: project-owner
  approved_at: 2026-08-12T00:00:00Z
  content_sha256: <canonical-plan-digest>
```

Plan 约束：

- 每个 requirement 至少有一个 criterion；
- 每个 criterion 有 statement 和至少一个 negative definition；
- implementation task 并集必须覆盖全部 criterion；
- acceptance owner 必须依赖实现任务；
- approval digest 绑定所有语义字段；
- Agent 可以生成 draft，但只能项目所有者改变 `draft -> approved`；
- approval 身份必须来自受信配置，不接受自由字符串冒充授权。

## TaskSpec

```yaml
schema_version: 1
id: XT-101
title: Authenticated one-file transport
plan: DP-P1-DELIVERY
criteria:
  - CRIT-P1-EXACT-BYTES

depends_on:
  - XT-100

owned_paths:
  - native/src/transfer/**
  - native/tests/transfer/**

type: feature
workstream: native_core

risk:
  functionality: high
  security: critical
  performance: medium
  compatibility: high
  concurrency: high
  platform: medium
  persistence: high

tdd:
  mode: red_green
  gate: one_file_integration
  proof_paths:
    - native/tests/transfer/**
  oracle_paths:
    - protocol/testdata/v1/**
  failure_fingerprints:
    - "FAILED: exact accepted bytes are not committed"

delivery:
  commit_type: feat
  scope: transfer
  summary: implement authenticated one-file transport
  architecture_change:
    mode: replace
    modules:
      - transfer
```

TaskSpec 约束：

- dependencies 和 owned paths 只在 TaskSpec 定义；
- criteria 必须来自已批准 Plan；
- risk 不能低于 `risk-routing.yaml` 根据路径和模块计算的最低值；
- Gate 由 risk、impact 和 TDD 合同联合推导；
- task 不能内嵌 shell 命令；
- acceptance 类型不能拥有产品实现路径；
- placeholder 或未批准 Plan 的任务不可 claim。

## Gate Policy

```yaml
schema_version: 1

resource_groups:
  native_build:
    max_parallel: 1
  flutter_sdk:
    max_parallel: 1
  lightweight:
    max_parallel: 8

gates:
  transfer_unit:
    command:
      argv: [make, transfer-test]
      timeout_seconds: 300
    inputs:
      - native/src/transfer/**
      - native/tests/transfer/**
    resource_group: native_build
    platforms: [local, linux, macos, windows]
    cache: success_only

  native_test:
    aggregate:
      - transfer_unit
      - session_unit
      - storage_unit

  verify:
    aggregate:
      - governance
      - architecture
      - abi_compat
      - native_test
      - flutter_test
```

Gate 约束：

- 一个节点只能定义 `command` 或 `aggregate`；
- DAG 必须无环；
- command 使用 argv，不经 shell 重新解析；
- timeout、输出上限、资源组和平台必须明确；
- 聚合 Gate 不得重复执行叶子；
- specialized Gate 不能被同命令文本或通用 Gate 名称替代；
- failure、timeout、crash、skip、dirty tree 不进入缓存。

## Risk Routing

```yaml
schema_version: 1

path_rules:
  - paths:
      - native/include/xnn_transfer/c_api.h
    minimum_risk:
      compatibility: critical
    required_gates:
      - abi_compat
      - native_cross_platform

  - paths:
      - protocol/**
      - native/src/security/**
    minimum_risk:
      security: critical
    required_gates:
      - security_negative
      - sanitizer
      - protocol_vectors

phase_minimums:
  review:
    - governance
    - architecture
  queue:
    - commit_policy
  release:
    - full_matrix
```

仓库策略只允许提高风险和增加 Gate，TaskSpec 不能降低自动推导结果。

## State Event

```json
{
  "schema_version": 1,
  "task_id": "XT-101",
  "sequence": 4,
  "previous_event_sha256": "<digest>",
  "from": "active",
  "to": "queued",
  "reason": "reviewed_submission",
  "actor": {
    "kind": "user",
    "id": "reviewer-slug"
  },
  "task_spec_blob": "<git-object-id>",
  "plan_blob": "<git-object-id>",
  "submission_ref": "refs/heads/submit/XT-101",
  "created_at": "2026-08-12T00:00:00Z"
}
```

State 约束：

- sequence 严格递增；
- previous digest 构成每任务 append-only 链；
- ref 更新必须比较预期旧 object ID；
- actor 由 Git/CI 身份映射，不接受调用方自由填写；
- 非法状态转换拒绝写入；
- `done` 事件必须引用 acceptance attestation 和 published SHA。

## Submission Manifest

```json
{
  "schema_version": 1,
  "task_id": "XT-101",
  "attempt": 2,
  "base_sha": "<accepted-base>",
  "source_head": "<reviewed-head>",
  "source_commits": ["<sha-1>", "<sha-2>"],
  "payload_patch_sha256": "<digest>",
  "task_spec_blob": "<object-id>",
  "plan_blob": "<object-id>",
  "gate_policy_blob": "<object-id>",
  "review_attestation_sha256": "<digest>",
  "tdd_attestation_sha256": "<digest>",
  "created_at": "2026-08-12T00:00:00Z"
}
```

Submission 创建后不可变。payload、TaskSpec、Plan、Gate policy 或 proof
任一变化都产生新 attempt，并归档旧 submission。

## Gate Attestation

```json
{
  "schema_version": 1,
  "gate_id": "one_file_integration",
  "phase": "queue",
  "source_sha": "<candidate-sha>",
  "source_tree": "<tree-object-id>",
  "command_sha256": "<digest>",
  "policy_sha256": "<digest>",
  "toolchain_sha256": "<digest>",
  "environment_sha256": "<digest>",
  "platform": "linux-x86_64",
  "started_at": "2026-08-12T00:00:00Z",
  "duration_ns": 123456789,
  "outcome": "success",
  "skipped": false,
  "result_sha256": "<digest>",
  "artifact_sha256": "<digest>"
}
```

Cache key 第一版为：

```text
SHA256(
  gate_id
  + source_tree
  + command_sha256
  + policy_sha256
  + toolchain_sha256
  + environment_sha256
  + platform
  + isolation_mode
)
```

## Acceptance Attestation

```json
{
  "schema_version": 1,
  "task_id": "XT-101",
  "submission_sha256": "<digest>",
  "candidate_sha": "<delivery-commit>",
  "candidate_tree": "<tree-object-id>",
  "integration_base": "<accepted-parent>",
  "payload_patch_sha256": "<digest>",
  "workflow": {
    "repository": "blackfrog638/XnnTransfer",
    "workflow_path": ".github/workflows/merge-queue.yml",
    "workflow_blob": "<object-id>",
    "run_id": 12345,
    "run_attempt": 1,
    "head_sha": "<delivery-commit>",
    "conclusion": "success"
  },
  "required_gate_attestations": ["<digest>"],
  "criterion_evidence": ["<digest>"],
  "skipped_jobs": [],
  "created_by": "trusted-queue-worker",
  "created_at": "2026-08-12T00:00:00Z"
}
```

Acceptance 约束：

- candidate SHA、workflow head SHA 和 artifact source SHA 完全一致；
- required jobs、platform matrix 和 criterion evidence 完整；
- skipped、neutral、cancelled、stale 或 partial matrix 一律失败；
- attestation 不能授权自身未包含的 payload；
- 发布前后均校验 protected branch CAS；
- attestation 不修改 candidate，因此没有 SHA 自引用。
