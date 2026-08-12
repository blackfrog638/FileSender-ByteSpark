# Harness V2 Testing Strategy

## 目标

测试策略同时优化两个指标：

- 反馈速度：只执行当前风险和影响需要的 Gate；
- 发布可信度：最终候选满足完整的专项合同且证据不可伪造。

提速不能依赖 skip、缩小未证明的测试范围或把本地结果冒充远端矩阵。

## 验证层级

| 层级 | 触发 | 目标时间 | 内容 |
| --- | --- | --- | --- |
| Focused | 开发中 | 小于 2 分钟 | 当前模块或 criterion |
| Review | submission 前 | 小于 8 分钟 | focused + 风险 + 架构 |
| Queue | exact candidate | 小于 20 分钟 | required platform 和 evidence |
| Nightly | 定时 | 不限 PR 时间 | 全仓库、长 fuzz、cache bypass |
| Release | milestone | 明确预算 | E2E、性能、互操作和恢复 |

## 风险路由

### 文档与规划

执行：

- schema validation；
- internal link/reference validation；
- ADR status/decision consistency；
- commit policy；
- architecture 文档引用检查。

不执行 native、Flutter、packaging 或 fuzz，除非文档同时改变生成输入。

### Harness 与治理

执行：

- Harness unit tests；
- adversarial lifecycle fixtures；
- state CAS 和 crash recovery；
- Gate DAG cycle/dedup；
- queue train simulation；
- attestation forgery fixtures；
- architecture 和 commit policy。

旧 Harness 曾把治理变更送入三平台产品构建。V2 只有在治理变更影响产品
构建脚本、Gate 命令或 toolchain 时才增加对应产品 Gate。

### Flutter

执行：

- format；
- analyze；
- affected feature tests；
- architecture import checks；
- 需要 native boundary 时执行 bundle smoke。

纯 presentation 变更不默认重建三平台 native library。

### Native

执行：

- 受影响 CMake leaf；
- 受影响模块 unit/integration；
- ABI/architecture impact；
- Linux 快速构建。

platform risk 为 high/critical 或修改平台实现时，queue 增加对应平台。

### ABI、协议、安全和持久化

始终增加专项 Gate：

- ABI frozen caller 和 export/layout；
- parser/golden/vector compatibility；
- hostile input 和 negative tests；
- sanitizer；
- 持久格式版本和恢复；
- required cross-platform matrix。

通用 `verify` 不能替代这些 Gate。

## Gate DAG 示例

```text
verify
├── governance
├── architecture
├── abi_compat
├── native_test
│   ├── discovery_test
│   ├── session_test
│   ├── storage_test
│   └── transfer_test
└── flutter_test
    ├── flutter_analyze
    └── flutter_unit
```

任务要求 `transfer_test + security_test + verify` 时，planner 展开并执行：

```text
governance
architecture
abi_compat
discovery_test
session_test
storage_test
transfer_test
flutter_analyze
flutter_unit
security_test
```

`transfer_test` 只出现一次。

## 并行与资源

默认资源组：

| 资源组 | 本地并行度 | 说明 |
| --- | --- | --- |
| lightweight | 8 | schema、lint、纯 Python |
| native_build | 1 | 共享 CMake/vcpkg 输出 |
| native_test | 2 | 已完成构建后的测试 |
| flutter_sdk | 1 | pub/analyze/test |
| detached_git | 3 | proof/candidate worktree |
| network | 2 | GitHub API 和 artifact |

执行器必须：

- 输出稳定的 canonical plan；
- 对独立节点并发；
- 对共享 build artifact 串行生产、并行消费；
- 失败后取消未开始的依赖节点；
- 清理子进程和临时 worktree；
- 保留 bounded diagnostics。

## 缓存策略

第一阶段只缓存：

- success；
- no-skip；
- clean source tree；
- 相同完整 source tree；
- 相同 Gate policy、command、toolchain、environment 和 platform。

不缓存：

- security fuzz 随机运行；
- performance；
- nightly cache-bypass Gate；
- manual witness；
- 包含外部不可固定服务的结果；
- 失败、超时和基础设施错误。

第二阶段只有满足以下条件才允许输入级缓存：

1. 模块依赖图机械完整；
2. undeclared dependency negative fixture 存在；
3. Gate 输入变化 mutation 测试存在；
4. 至少一个月 nightly 未发现漏跑；
5. integration owner 批准策略变更。

## TDD 验证

Red attestation 测试覆盖：

- base 通过、Red 预期失败；
- 生产路径在任意历史 commit 出现时拒绝；
- compiler error/timeout/skip 不算 Red；
- oracle digest 变化使 proof 失效；
- failure fingerprint 必须是完整输出事件；
- feature、bugfix、refactor、test 和 governance 模式正确路由；
- documentation/acceptance 不被强迫制造 Red。

review 不默认重放 Red，但必须验证 Red attestation 的签名、输入和 frozen
oracle。高/critical 风险可以由 policy 强制远端重放，不允许 TaskSpec
自行关闭。

## Queue 验证

Queue 测试分三类：

### 纯模型测试

- dependency ordering；
- owned-path conflict；
- train prefix；
- candidate parent；
- state transitions；
- retry budget。

### 临时 Git 仓库集成测试

- immutable submission；
- squash patch equivalence；
- CAS race；
- failed-prefix rebuild；
- archive ref；
- crash recovery；
- dirty worktree cleanup refusal。

### 远端合同测试

- exact workflow identity；
- wrong SHA/run/attempt；
- missing/skipped jobs；
- partial matrix；
- stale/expired artifact；
- publication race；
- branch protection denial。

远端合同测试使用 fake HTTP server 和 recorded fixtures；少量 live smoke
在专用测试仓库执行，不能对生产 `harness` 分支做破坏性测试。

## CI 工作流

### `review.yml`

- PR 和 submission review 触发；
- 运行 impact-selected review Gate；
- 目标 P50 小于 8 分钟；
- 不发布产品；
- 输出 review Gate attestations。

### `merge-queue.yml`

- 只对 `queue/**` candidate ref 触发；
- 运行 exact candidate required Gate；
- 生成 criterion artifacts；
- 不直接 push protected branch；
- queue worker 验证后发布。

### `nightly.yml`

- 完整 matrix；
- cache bypass；
- 长 fuzz；
- dependency drift；
- Harness mutation/adversarial suite；
- 输出趋势，不自动接受失败任务。

## Harness 自身质量门

V2 实现必须满足：

- Python type checking；
- formatter/linter；
- unit tests；
- 临时 Git 仓库 integration tests；
- mutation/adversarial tests；
- branch/ref crash recovery；
- macOS、Linux、Windows 路径语义；
- 关键状态机 property-based tests；
- 不低于旧 ABI、architecture 和 security Gate 的覆盖。

覆盖率只作为可见性指标。发布要求由行为和负例组成，不以单一百分比代替。

## 性能基准

实现阶段建立固定 benchmark fixture：

- 100 个 TaskSpec 的 validate；
- 20 个 active/queued state refs 的 list；
- 200-node Gate DAG 展开；
- 10 个独立 lightweight Gate 调度；
- 3-task merge train candidate 构造；
- 1,000 条 state event replay；
- cache hit/miss；
- crash recovery。

每个阶段记录 wall time、CPU、子进程数、Git 命令数和网络请求数。性能
回归阈值在三轮稳定基线后批准，不能在没有方差数据时拍脑袋设置。
