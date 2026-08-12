# Harness V2 Architecture

## 设计原则

1. 静态契约进入产品仓库，运行状态不进入产品历史。
2. 一项事实只有一个权威来源，其他视图全部派生。
3. 验证按风险和影响选择，不按生命周期阶段重复执行。
4. 开发、验证和发布解耦；只有受保护分支更新需要串行。
5. 失败历史可审计，但 accepted branch 不携带失败 lineage。
6. V2 原位替换旧 Harness，不长期维护两个生产实现。

## 目标目录

```text
XnnTransfer/
├── .agents/
│   ├── manifest.yaml
│   ├── gates.yaml
│   ├── risk-routing.yaml
│   ├── plans/
│   │   └── DP-*.yaml
│   ├── tasks/
│   │   └── XT-NNN.yaml
│   └── schemas/
│       ├── plan.schema.json
│       ├── task.schema.json
│       ├── gate.schema.json
│       └── attestation.schema.json
│
├── tool/harness/
│   ├── agent.py
│   ├── model.py
│   ├── validate.py
│   ├── git_ops.py
│   ├── workspace.py
│   ├── state.py
│   ├── gates.py
│   ├── executor.py
│   ├── tdd.py
│   ├── queue.py
│   ├── attestation.py
│   ├── dashboard.py
│   └── tests/
│
└── .github/workflows/
    ├── review.yml
    ├── merge-queue.yml
    └── nightly.yml
```

实现时优先保持模块少而完整。只有当一个文件超过清晰的责任边界时，
才拆分子包；不得把当前 47 个脚本机械迁移成 47 个 Python 模块。

## 三个存储平面

### 版本化契约平面

保存在产品仓库并参与 code review：

- `.agents/manifest.yaml`：V2 版本和全局策略；
- `.agents/plans/`：需求、criterion、负例、验收 owner；
- `.agents/tasks/`：任务依赖、owned paths、风险和交付合同；
- `.agents/gates.yaml`：受信命令、Gate DAG、输入和资源组；
- `.agents/risk-routing.yaml`：风险到 Gate 的最低映射；
- `.agents/schemas/`：所有机器可读格式。

版本化契约不包含 owner、runtime state、candidate SHA、CI URL 或时间戳。

### 远端运行控制平面

每个 ref 都指向不可变 JSON event 或 manifest commit：

```text
refs/heads/state/XT-101
refs/heads/submit/XT-101
refs/heads/queue/train-000042/001
refs/heads/queue/train-000042/002
refs/heads/attest/XT-101
refs/heads/archive/XT-101/<attempt>
```

约束：

- `state/*` 只能 compare-and-swap；
- `submit/*` 创建后不可重写，同一任务新评审产生新 attempt；
- `queue/*` 是临时候选，可由 worker 回收；
- `attest/*` 只允许受信 CI/queue 身份创建；
- `archive/*` 禁止删除，按保留策略压缩或转存。

具体 ref 命名在实现前通过托管平台能力测试确认。若平台不允许目标
namespace，可映射为受保护的同名 branch，但语义不变。

### 本地临时平面

位于 common Git directory，不进入工作树：

```text
.git/xnn-harness/
├── cache/<evidence-key>/
├── locks/
├── timing.jsonl
├── state-snapshots/
└── worktrees/
```

这些数据可以删除重建，不能单独使任务进入 `done`。

## 组件边界

```text
agent.py
  |
  +-> model.py / validate.py
  +-> workspace.py -----> git_ops.py
  +-> state.py ---------> git_ops.py
  +-> gates.py ---------> executor.py
  +-> tdd.py -----------> executor.py + attestation.py
  +-> queue.py ---------> git_ops.py + executor.py + attestation.py
  +-> dashboard.py -----> read-only ports
```

规则：

- `model.py` 不执行 Git、进程或网络操作；
- `git_ops.py` 不解释业务状态；
- `executor.py` 只执行解析后的受信 Gate；
- `queue.py` 不能自行修改 TaskSpec 或 Gate policy；
- `attestation.py` 不运行产品命令，只验证和封装结果；
- `dashboard.py` 只读，不写任何权威状态。

## 生命周期

V2 只保留四个持久状态：

```text
ready -> active -> queued -> done
           ^          |
           +----------+
```

`queued -> active` 只发生在需要修改 source payload 时。

以下内容是状态事件的 reason，不是额外状态：

- blocked dependency；
- waiting review；
- waiting CI；
- infrastructure retry；
- publication retry。

这样可以避免 `claimed`、`in_progress`、`review`、`integrated` 和多个本地
缓存状态相互漂移。

## 开发与所有权

`task claim` 对 `state/<task>` 执行 CAS，并创建 worktree。claim 校验：

- TaskSpec 和 Plan 已批准；
- dependencies 已发布；
- owned paths 不与 `active` 或 `queued` 任务相交；
- base 是 accepted branch 的祖先；
- worktree 目标不存在；
- TaskSpec 不包含 unresolved placeholder。

任务进入 `queued` 后释放开发 Agent 和 worktree，但 owned paths 继续保留。
只有 `done` 或显式撤销 submission 才释放路径。

## Gate 执行

Gate planner 读取所有风险和阶段要求，得到唯一叶子集合：

```text
TaskSpec gates
  + risk-routing gates
  + phase minimum gates
  + changed-path impact gates
        |
        v
  DAG expand -> deduplicate -> resource schedule
```

第一版证据缓存绑定完整 source tree，优先保证正确性。只有当架构依赖图、
正负 fixture 和 cache poisoning 测试都完备后，才允许按声明输入缩窄键。

## Merge train

Queue worker 从当前 accepted branch 构造链式候选：

```text
H0 -- A -- B -- C
      |    |    |
     CI-A CI-B CI-C
```

- A、B、C 是独立 reviewed submission；
- 每个候选 SHA 精确绑定其累计树；
- CI 可以并发；
- 发布仍按 A、B、C 进行短 CAS；
- A 失败时，B 和 C 的 payload 从 H0 重建新 train；
- 不在包含 A 失败历史的 lineage 上 rebase。

Train 只接收 owned paths 不相交、依赖顺序满足且 Gate 资源预算允许的任务。

## 验收与发布

```text
candidate CI success
        |
        v
validate jobs/artifacts/no-skip/criteria
        |
        v
create acceptance attestation
        |
        v
CAS protected branch
        |
        v
append published state event -> done
```

如果 attestation 已创建但 CAS 失败，任务保持 `queued`。如果 CAS 成功但
最终 state event 写入失败，恢复器通过 protected branch 和 attestation
派生并补写事件。任何情况都不创建产品 acceptance commit。

## 与产品架构的隔离

Harness V2 是工程控制面，不能成为产品运行时依赖：

- `native/`、`apps/desktop/` 和协议实现不能导入 Harness 代码；
- Harness 可以读取架构 inventory，但不能绕过其边界；
- Harness schema 不是产品持久格式或 wire protocol；
- Harness 故障不能改变已发布产品 commit；
- 删除 `.git/xnn-harness/` 不影响产品构建和历史。
