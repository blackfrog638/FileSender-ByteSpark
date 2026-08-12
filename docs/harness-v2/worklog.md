# Harness V2 Bootstrap Worklog

本文件记录 bootstrap 期间的人工授权、重要决策、命令、验证和残余风险。
它是审计日志，不是任务运行状态来源。

## 授权

### 2026-08-12：启动规划

项目所有者要求：

- 按新的 Harness V2 架构开始计划；
- 本次重构使用快速通道；
- 不要求旧 Harness 批准对旧 Harness 的替换；
- 计划和文档必须完整；
- 工作过程必须保留可审计痕迹。

执行边界：

- 当前只进行规划，不实现 V2 runtime；
- 不调用旧 `agent.sh` 生命周期；
- 不修改当前 `harness`；
- 不清理旧 branch/worktree/ref；
- ADR 保持 proposed，等待项目所有者评审。

### 2026-08-12：批准实施

项目所有者在完整规划发布后明确指示：

```text
直接开始实行
```

该指令确认：

- 接受 ADR 0017 的 V2 方向；
- 允许状态和 attestation 移出产品历史；
- 允许一次 candidate CI 取代双 CI；
- 允许 speculative merge train；
- 允许 V2 原子替换 V1；
- 继续使用快速通道，不创建旧 XT/record；
- 迁移 trusted base 使用规划开始时的远端
  `origin/harness@5fa1a864e2304aea818aa7fb6d0a193a48fe67aa`。

ADR 已改为 accepted，实施计划已改为 active。Pilot 具体内容在 WP8 前
选择，不阻塞基础实现。

## 基线

| 项目 | 值 |
| --- | --- |
| 仓库 | `https://github.com/blackfrog638/XnnTransfer.git` |
| 本地 harness | `61bbdda590d4009dbcaa378ef827d0a7bf99aeff` |
| origin/harness | `5fa1a864e2304aea818aa7fb6d0a193a48fe67aa` |
| 规划分支 | `bootstrap/harness-v2` |
| 规划 worktree | `/Users/bytedance/XnnTransfer/XnnTransfer-HarnessV2` |
| 日期 | `2026-08-12` |

注意：规划基线不是迁移 trusted product base。后者必须由项目所有者在
阅读迁移审计后明确选择。

## 初始状态观察

- `tool/harness` 有 47 个 Python/Shell 文件，约 2.2 万行；
- 活跃治理元数据约 2.4 万行；
- durable records 包含 67 个 `done`、20 个 `ready`；
- task list 仍将 XT-097 branch record 解析为 `in_progress`；
- 存在 XT-070、XT-083、XT-093、XT-097 task worktree；
- 存在 async-delivery、schema4 fixture、transport composition plan worktree；
- 存在 detached XT-097 diagnostic worktree；
- local harness 比 origin/harness 多一个提交；
- 代表性完整 CI 的关键路径约 15 分钟；
- XT-082 delivery candidate CI 约 20 分钟，acceptance candidate CI
  约 6 分钟，随后 protected branch push 仍会触发 workflow。

## 决策记录

| ID | 状态 | 决策 |
| --- | --- | --- |
| D-001 | confirmed | 使用独立 bootstrap branch/worktree，不修改当前 harness |
| D-002 | confirmed | 规划阶段绕过旧 XT/schema-v4 生命周期 |
| D-003 | confirmed | 静态契约只保留 Plan、TaskSpec 和 Gate policy |
| D-004 | confirmed | 运行状态迁移到每任务独立远端 state ref |
| D-005 | confirmed | Gate DAG 去重并并行执行 |
| D-006 | confirmed | Red attestation 可信时不在 review 默认重放 |
| D-007 | confirmed | 使用 immutable submission 和 speculative merge train |
| D-008 | confirmed | 一次 candidate CI + acceptance attestation |
| D-009 | confirmed | 删除 record-only acceptance commit 和全局 integrated slot |
| D-010 | confirmed | V2 原子替换 V1，不长期双写 |

`confirmed` 表示项目所有者已授权本次 bootstrap 使用该决策。后续新增
或改变公共治理合同，仍需新的明确批准。

### 2026-08-12：V1 冻结和归档

检查结果：

- 11 个相关 worktree 全部 clean；
- GitHub 没有 queued 或 in-progress workflow；
- `origin/harness` 是 local `harness` 的祖先；
- local `harness` 比远端多一个旧治理提交。

创建并推送：

```text
archive/harness-v1/protected-base
archive/harness-v1/final-local
archive/harness-v1/task/XT-070
archive/harness-v1/task/XT-083
archive/harness-v1/task/XT-093
archive/harness-v1/task/XT-097
archive/harness-v1/plan/async-delivery
archive/harness-v1/plan/async-delivery-v3
archive/harness-v1/plan/schema4-lifecycle-fixture-v2
archive/harness-v1/plan/p1-transport-composition
archive/harness-v1/diagnostic/XT-097-red
```

所有远端 ref 已用 `git ls-remote` 校验精确 SHA。原 branch 和 worktree
仍然保留，未执行 cleanup。

### 2026-08-12：WP1/WP2 契约、State 与 Workspace

实现：

```text
tool/harness/model.py
tool/harness/validate.py
tool/harness/git_ops.py
tool/harness/state.py
tool/harness/workspace.py
tool/harness/tests/test_model.py
tool/harness/tests/test_state_workspace.py
make harness-v2-test
```

实现决策：

- 静态契约使用标准库可解析的严格 JSON；
- 不依赖本机偶然安装的 PyYAML；
- 重复 JSON key、未知字段和未规范化 ID fail closed；
- Plan approval 从仓库配置的项目所有者 Git identity 解析，不接受
  `--by` 自由文本；
- State event 使用 Git object plumbing 写入独立 ref；
- 每任务 ref 使用 append-only parent chain 和 compare-and-swap；
- 远端 state push 使用 `--force-with-lease`，失败时回滚本地 ref；
- Claim 失败会追加 `active -> ready` rollback event，不留下半 claim；
- 只有相关 owned path 或绑定治理输入变化才判定 stale。

验证：

```text
python3 -B -m unittest discover -s tool/harness/tests -p 'test_*.py' -v
python3 -m py_compile <V2 modules and tests>
git diff --check
```

结果：

- 21 项测试全部通过；
- 覆盖 approval spoof、stale digest、task command injection、Gate cycle、
  risk downgrade、criterion closure、state tamper、CAS、remote state、
  claim、dependency、rollback 和 stale path；
- Python compile 和 whitespace 检查通过；
- 未调用旧 Harness runtime；
- 未修改 protected branch。

### 2026-08-12：WP3/WP4 Gate DAG、Executor 与 TDD

实现：

```text
tool/harness/gates.py
tool/harness/executor.py
tool/harness/tdd.py
tool/harness/tests/test_gates_tdd.py
```

行为：

- Task criterion、TDD、risk routing 和 phase minimum 合并后展开 DAG；
- aggregate Gate 只贡献依赖，leaf 在一个 plan 中只执行一次；
- command 使用 argv 直接执行，不经过 shell；
- independent resource groups 并行，shared group 使用 semaphore；
- 输出写临时文件并计算完整 SHA-256，只保留 bounded diagnostic；
- timeout 杀死进程组；
- success/no-skip 才写本地 cache；
- cache key 绑定 source tree、command、policy、toolchain、environment、
  platform 和 isolation，不绑定 commit SHA；
- 同一 source tree 的新 commit 可复用证据，但新 attestation 仍绑定当前
  source SHA 并记录 `reused_from_source_sha`；
- Red 创建时执行 base 和 Red 一次；
- 重复 `record_red` 在任何 Gate 执行前返回 immutable attestation；
- review 验证 frozen proof/oracle 后只执行 Green；
- production-before-Red、infrastructure failure 和 post-Red oracle change
  全部拒绝。

验证：

```text
make harness-v2-test
```

首轮 31 项中 29 项通过。失败项分别证明 duplicate command 检查和 Red
非幂等问题。修复 fixture 命令身份并将已有 attestation 检查提前后，
31 项全部通过。

### 2026-08-12：WP5/WP6 Submission、Merge Train 与 Publication

实现：

```text
tool/harness/attestation.py
tool/harness/merge_queue.py
tool/harness/github_evidence.py
tool/harness/agent.py
tool/harness/tests/test_queue_attestation.py
tool/harness/tests/test_github_evidence.py
tool/harness/tests/test_agent_cli.py
.github/workflows/review.yml
.github/workflows/merge-queue.yml
.github/workflows/nightly.yml
```

行为：

- 独立 reviewer 生成 immutable submission ref；
- high/critical task owner 不能同时充当 reviewer；
- submission 绑定 source range、payload SHA-256、Plan、TaskSpec、Gate、
  TDD 和 review attestations；
- queued 后开发 worktree 释放，owned paths 继续保留；
- merge train 构造 `base -> A -> A+B` 累积候选；
- 每个 candidate patch 必须与对应 submission byte-equivalent；
- queue candidate ref 可独立并行触发 CI；
- GitHub collector 从 credential helper 获取 token；
- hosted evidence 绑定 repository、workflow path/blob、run/attempt、branch、
  candidate SHA、jobs 和下载后的 artifact；
- skipped、wrong SHA、missing job、stale artifact、unsafe ZIP 和 incomplete
  pagination fail closed；
- acceptance attestation 在 candidate 外部 ref 保存；
- protected branch 只做 parent-matched CAS；
- publish 成功但 state finalization 失败可以从 branch + attestation 恢复；
- 单一 `agent.py` 提供 validate/list/claim/TDD/verify/submit/queue/publish/
  recover 命令；
- queue platform matrix 由 TaskSpec 风险机械选择。

验证：

```text
make harness-v2-test
python3 -m py_compile <WP5/WP6 modules and tests>
PyYAML safe_load .github/workflows/*.yml
git diff --check
```

结果：

- 首次加入队列模块后发现 `queue.py` 覆盖 Python 标准库 `queue`；
- 模块改名为 `merge_queue.py`，未使用伪装标准库接口的兼容补丁；
- 增加真实双任务 train 和 no-skip 负例后，共 46 项测试全部通过；
- 4 个 workflow YAML 语法解析通过；
- `actionlint` 本机不可用，未执行；CI/live smoke 在 WP8 验证；
- 未修改 protected branch。

## 命令日志

### 2026-08-12：现状读取

只读命令：

```text
git status --short --branch
tool/harness/agent.sh list
git worktree list --porcelain
git for-each-ref ...
wc -l tool/harness/*
gh run list ...
gh run view ...
```

结果：

- 确认效率瓶颈来自重复 Gate、双 CI、单 integrated slot 和 tracked state；
- 确认已有 async-delivery draft 仍在 schema-v4 上叠加兼容层；
- 未修改任何旧任务、record、branch 或 worktree。

### 2026-08-12：创建规划工作区

命令：

```text
git worktree add -b bootstrap/harness-v2 \
  /Users/bytedance/XnnTransfer/XnnTransfer-HarnessV2 \
  61bbdda590d4009dbcaa378ef827d0a7bf99aeff
```

结果：成功创建独立 planning worktree。

### 2026-08-12：规划文档

创建：

```text
docs/adr/0017-harness-v2-control-plane.md
docs/harness-v2/README.md
docs/harness-v2/architecture.md
docs/harness-v2/contracts.md
docs/harness-v2/delivery-flow.md
docs/harness-v2/threat-model.md
docs/harness-v2/testing-strategy.md
docs/harness-v2/migration.md
docs/harness-v2/implementation-plan.md
docs/harness-v2/acceptance-checklist.md
docs/harness-v2/worklog.md
```

结果：文档已完成，并进入独立 planning commit。

### 2026-08-12：文档验证

命令：

```text
git diff --cached --check
内部 Markdown 相对链接检查
文档索引与一级标题检查
批准状态和旧生命周期调用审计
```

结果：

- 11 个规划文件、2,495 行新增内容进入 staged diff；
- whitespace/diff 检查通过；
- 内部 Markdown 链接检查通过；
- ADR 保持 `proposed`；
- 实施计划保持 `draft` 和“执行授权未获得”；
- 未发现调用旧生命周期推进本次 bootstrap 的指令；
- `make verify` 未运行：本次只有规划文档，且快速通道明确不使用旧
  Harness 对 V2 规划进行自审批。

### 2026-08-12：规划发布

规划内容提交：

```text
9fa059d95230ea4bd773bef90c36475fca21026c
docs(harness): define v2 bootstrap plan
```

发布命令：

```text
git push -u origin bootstrap/harness-v2
git ls-remote --heads origin refs/heads/bootstrap/harness-v2
```

结果：

- 远端 `bootstrap/harness-v2` 创建成功；
- 首次发布 ref 精确指向规划内容提交 `9fa059d`；
- 未修改或发布到 `harness`；
- 分支名不匹配现有 `main`、`harness`、`ci/**` workflow push 条件，
  因此没有调用旧 Harness 验收。

## 待项目所有者确认

1. Trusted product base SHA；
2. state/attestation 移出产品历史；
3. 一次 candidate CI 替代双 CI；
4. speculative merge train；
5. queue worker 身份与权限；
6. 两个 pilot task；
7. ADR 0017 是否接受。

## 残余风险

- 当前旧 worktree 尚未归档或清理；
- planning branch 基于 local harness，不代表最终迁移基线；
- GitHub 对目标 ref namespace 和 branch protection 的能力尚未 live 验证；
- RFC 8785 canonicalization 依赖尚未选型；
- queue worker credential 和签名方案尚未决定；
- 文档中的 SLO 需要实现后的实际基线确认；
- V1 删除清单需要 dependency search 验证。

## 后续日志规则

每个工作包完成时追加：

- source base/head；
- changed paths；
- contract changes；
- commands 和结果；
- skipped Gate 及原因；
- reviewer；
- residual risk；
- rollback point；
- 项目所有者阶段门决定。

禁止覆盖历史日志条目；更正使用新条目说明。

## 2026-08-12：HV2-WP7 原子替换候选

基线：

```text
source: 89a8b252785e3627524f0d7b66e972e87f07cfbd
branch: bootstrap/harness-v2
rollback: origin/bootstrap/harness-v2 at 89a8b252
```

实施结果：

- 删除 V1 backlog、records、handoffs、schema-v4 task specs、runtime
  writers、旧 CI workflow 和并行 provider；
- 写入只读 migration snapshot：67 accepted、20 deferred；
- 激活 strict Plan/TaskSpec/Gate contracts、CAS state refs、immutable
  submissions、cumulative merge train、external attestations；
- 增加 remote immutable Plan approval ref，production claim/queue/publish
  必须读取 owner approval；
- 增加 `claim-recover`、`queue-reopen`、publication recovery 和
  payload-free `acceptance-close`；
- submission commit 以 source head 为 parent，fresh queue worker 可从
  remote submission ref 取得 source objects；
- publisher 重算 candidate parent、payload、workflow、platform Gate 和
  criterion closure，并按 train predecessor state 顺序发布；
- executor 检测 unittest/pytest/CTest/GTest/Flutter skip 输出，skip 不成功、
  不缓存、不能作为 TDD Red；
- criterion evidence 绑定完整 criterion contract、candidate SHA 和 Gate
  attestation digests；
- trust-root changes 不能通过普通 task queue 自行授权。

恢复测试覆盖：

- claim 在 state CAS 后崩溃；
- fresh worker 缺少本地 source/state/submission objects；
- queue CI 失败后 archive/reopen；
- predecessor 未持久化 done；
- acceptance ref 已写但 protected CAS 失败后重试；
- protected CAS 成功但 state finalization 失败；
- acceptance owner 零 payload criterion closure。

本地命令和结果：

```text
python3 -B tool/harness/agent.py validate
  PASS: gates=12, plans=0, tasks=0
make harness-v2-test
  PASS: 74 tests (final aggregate run)
make architecture-test
  PASS: 17 tests and dependency matrix
make commit-message-test
  PASS: 19 tests and current range
make contract-test
  PASS
make abi-compat-test
  PASS: 4/4
make security-test
  PASS: sanitizer CTest 27/27 and both bounded fuzzers
make native-test
  PASS: CTest 28/28
make flutter-test
  PASS: analyze clean, 53 tests
make dependency-test
  PASS: 30 policy tests and pinned dependency probe
make diff-check
  PASS
python3 -B -m py_compile tool/harness/*.py tool/harness/tests/*.py
  PASS
PyYAML safe_load .github/workflows/*.yml
  PASS
git diff --check
  PASS
```

首次 ABI/security 执行在 configure 前失败，因为新 worktree 不存在 pinned
vcpkg。执行 `make vcpkg-bootstrap` 后重新运行，两项均通过；该首次结果不
计为产品 Gate failure。

Reviewer/授权：

- 项目所有者已明确接受 ADR 0017 并指示直接实施；
- 本地实现完成自审和 adversarial suite；
- exact candidate 的 hosted reviewer/CI evidence 尚未生成。

保留的 archive/rollback：

- `archive/harness-v1/final-local`；
- `archive/harness-v1/protected-base`；
- 全部已盘点 V1 task/plan/diagnostic refs；
- cutover commit 前远端 bootstrap rollback point `89a8b252`。

未完成和残余风险：

- `actionlint`、JSON Schema meta-validator 本机不可用；workflow 已通过
  PyYAML，runtime schema 通过 semantic validator；
- Linux/macOS/Windows exact candidate CI 尚未运行；
- `approve/**`、state/submit/queue/attest/archive ruleset 与 queue-worker
  最小权限尚未在远端审计；
- publication slot 小于 5 秒、cache P50/P95 和吞吐 SLO 尚未 benchmark；
- Pilot A/Pilot B 尚未注册和交付；
- protected `harness` 尚未执行最终 CAS，V1 archive 在发布完成前不清理。

## 2026-08-12：HV2-WP8 本地 aggregate 与 cache baseline

原子切换提交：

```text
ec342d2 feat(harness): replace v1 control plane atomically
338 files changed, 6141 insertions, 43518 deletions
```

在 clean tree 上执行 V2 completion gate：

```text
/usr/bin/time -p make verify
  PASS: 9 unique leaf Gate attestations
  cold real: 93.64s

/usr/bin/time -p make verify
  PASS: same plan, success/no-skip cache reused
  warm real: 0.27s
```

冷缓存单次 baseline 已低于 2 分钟目标，热缓存证明相同 tree 的 leaf Gate
未重复构建。该数据不是跨任务 P50/P95；正式 SLO 仍需 pilot 样本。

本地最终结果：

- Harness V2 suite：74/74；
- native CTest：28/28；
- sanitizer CTest：27/27；
- ABI compatibility：4/4；
- Flutter analyze：0 issues；
- Flutter tests：53 passed；
- dependency policy：30 tests + pinned probe；
- architecture policy：17 tests + dependency matrix；
- `make verify`：passed。

下一阶段只接受 exact hosted evidence：push 后的 Linux/macOS/Windows
workflow、remote namespace ruleset、queue-worker credential boundary、CAS
publication timing 和两个 pilot。它们不能由以上本地结果替代。

## 2026-08-12：Bootstrap exact-candidate 路径

审计发现普通 merge queue 不能合法承载 control-plane cutover：

- active Plan/TaskSpec catalogue 为空；
- cutover 修改 workflow、schemas、Harness 和其他 trust roots；
- 给 bootstrap commit 添加虚构 `Xnn-Task` trailer 会伪造产品 provenance。

因此在现有 merge-queue workflow 中增加受限的
`queue/bootstrap/**` 模式：

- matrix 固定为 Linux、macOS、Windows；
- 每个平台执行 `verify-all --no-cache`；
- artifact 使用独立 `bootstrap_cutover` schema，不包含产品 criterion；
- Linux 额外执行 sanitizer 和 bounded fuzz；
- 普通 `queue/**` 继续从 candidate TaskSpecs 推导 matrix 和 criterion
  evidence，不接受 bootstrap artifact。

该模式只用于 ADR 0017 原子切换。Hosted run、artifact digest、branch
protection 更新、bootstrap attestation 和 protected CAS 结果将在 run
完成后追加。

### Hosted attempt 1

```text
candidate: 08b4b51f3038bd5ff2a28a4c769e1e5214619741
run: https://github.com/blackfrog638/XnnTransfer/actions/runs/31574684639
result: failed/cancelled after decisive failures
```

- Candidate plan 与 Harness V2 jobs 通过；
- macOS product Gate 在 fresh runner 缺少 pinned vcpkg，native leaf 失败，
  Flutter 在 native library 不存在时报告 skip；
- Windows runner 的 `PATH` 优先解析系统 `bash.exe`，所有 shell leaf
  失败；
- executor 当时只输出 outcome，未把已收集的 bounded diagnostic 带入
  CI failure summary；
- run 已无法作为 acceptance evidence，主动取消剩余 Linux/security
  资源，未重用其部分结果。

修复：

- product job 在 executor 前 bootstrap pinned vcpkg；
- bootstrap candidate 预建 native core，Flutter cross-layer tests 不依赖
  并发 Gate 完成顺序；
- Windows 将 Git Bash 目录写入 `GITHUB_PATH`；
- `require_success` 输出每个失败 leaf 最后 4 KiB trusted diagnostic；
- failed candidate 保留 archive ref，后续 attempt 使用新 SHA 和完整新 run。

### Branch protection context migration

远端 `harness` 在 cutover 前启用 `strict`、`enforce_admins`、禁止 force
push 和删除，并要求 11 个 V1 workflow contexts。V2 的 platform matrix
由 TaskSpec criterion 和 risk routing 动态展开，不能把三个静态 platform
job 名直接设为 required contexts，否则单平台合法 candidate 会永久阻塞。

V2 workflow 增加稳定聚合 context `Candidate accepted`：

- `Candidate plan`、`Harness V2` 和动态 `Product gates` 必须全部成功；
- `queue/bootstrap/**` 还必须要求 `Cutover security` 成功；
- 普通 queue 明确要求一次性 security job 为 `skipped`；
- publisher 仍逐项验证 run identity、workflow blob、matrix、Gate
  attestation 和 criterion artifact，聚合 context 不替代内容验证。

最终 bootstrap run 通过后，远端 protection required context 将原子替换
为 `Candidate accepted`，其余 strict/admin/force/delete 设置保持不降级。

### Hosted attempt 2

```text
candidate: 86ddae3142fb3198fe3aac488b82e80a96663b61
run: https://github.com/blackfrog638/XnnTransfer/actions/runs/31575945207
result: failed with Linux and security qualification complete
```

- Candidate plan、Harness V2、Linux exact product Gate 和 Cutover security
  全部通过；
- Windows 已确认 Git Bash 生效，随后 `msvc-dev-cmd` 注入 Visual Studio
  bundled `VCPKG_ROOT`，覆盖仓库 pinned checkout；
- macOS 完成 pinned dependencies 和 native core 构建，唯一失败为并发
  测试的 `0.65s` 墙钟阈值，两个 `0.35s` leaf 实测 `0.663s`；
- bounded diagnostic 将 macOS 失败 leaf 和断言完整带到 job log，证明
  attempt 1 的可观测性修复生效。

修复：

- product bootstrap 将仓库 checkout 以更高优先级的
  `XNN_TRANSFER_VCPKG_ROOT` 写入后续 step 环境；
- 并发测试改为记录两个子进程的 monotonic start/end 并断言区间重叠，
  不再把 hosted runner 调度延迟误判为串行；
- 增加 `Candidate accepted` 稳定聚合 context；
- attempt 2 candidate 保留 archive ref，Linux/security 部分结果不拼接
  到后续 candidate acceptance。

### Bootstrap closure hardening

Branch protection 审计同时发现两个 pre-publication 缺口：

1. 计划声明 `attest/bootstrap/<SHA>`，但当时只有平台 evidence schema，
   没有 hosted artifact collector、acceptance schema 或 immutable store；
2. 普通 queue 的 cutover-only security job 会以 job-level `skipped`
   出现，违反 acceptance collector 的 no-skip 规则。

修复：

- `bootstrap-accept` 绑定 repository、workflow blob、run/attempt、全部成功
  jobs、candidate SHA/tree、三个 artifact archive digest、统一 global Gate
  plan 和逐平台 leaf 集合；
- immutable `bootstrap_acceptance` 写入
  `refs/heads/attest/bootstrap/<candidate-sha>`，自由 approver 参数不可用；
- `bootstrap-publish` 在发布前从远端重读 attestation 并重算合同绑定，
  只允许 attested base 到 candidate 的 protected CAS，随后 readback；
- cutover security job 对普通 queue 运行非 Gate policy marker 并成功结束，
  bootstrap ref 才运行 sanitizer/fuzz，因此普通 workflow 不产生 skipped
  job；
- bootstrap 平台 artifacts 改为共享 global Gate plan digest，平台差异只
  由各自 `gate_ids` 表示。

早于该修复的 hosted attempt 仅作为诊断/平台资格记录，不能生成最终
bootstrap acceptance。

### Hosted attempt 3

```text
candidate: 69bbb9985087b6cf6e954c65782cdcd6c482a3da
run: https://github.com/blackfrog638/XnnTransfer/actions/runs/31577702315
result: failed/cancelled after Windows failure
```

- macOS 和 Linux exact product Gates 全部通过并上传 evidence；
- Windows pinned vcpkg、native core 和 Git Bash workflow shell 均通过，
  但 executor 的裸 `bash` 子进程仍解析到 Windows WSL launcher，所有
  Bash leaf 报告“Windows Subsystem for Linux has no installed
  distributions”；
- candidate 已因 bootstrap closure hardening 发生 trust-root 变更而失效，
  Windows 失败后取消剩余 security job，未拼接部分 evidence。

修复：

- workflow 写入绝对 `XNN_TRANSFER_BASH`，不再依赖 MSVC action 重建的
  Windows `Path` 顺序；
- executor 校验 override 为绝对文件路径，将 resolved argv 同时用于
  toolchain digest 和实际 `Popen`，防止 attestation 与执行二进制不一致；
- 新增 override path 与 executable-content digest 测试。
