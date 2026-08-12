# Harness V2 Threat Model

## 保护资产

- 项目所有者批准的 requirement 和 criterion 语义；
- TaskSpec 的依赖、owned paths、风险和验证要求；
- reviewed source payload；
- Red/Green 测试因果关系；
- Gate 命令、工具链和执行结果；
- candidate source、binary 和 artifact；
- protected branch 的 accepted lineage；
- state event 和 acceptance attestation；
- GitHub credential、签名密钥和用户隐私数据。

## 参与者

| 参与者 | 默认信任 |
| --- | --- |
| 项目所有者 | 可批准需求、ADR 和例外 |
| Integration owner | 可管理策略和 queue worker |
| Task Agent | 不受信；只能修改 owned paths |
| Reviewer Agent/用户 | 受限信任；不能发布 |
| Queue worker | 受信发布主体，权限最小化 |
| CI runner | 对一次隔离执行受信，不可批准需求 |
| GitHub | 托管、身份和 protected-ref 边界 |
| LAN/外部输入 | 完全不受信 |

## 信任边界

```text
Task worktree
    | untrusted payload
    v
Review + Gate executor
    | immutable submission
    v
Queue candidate
    | exact-SHA CI attestations
    v
Queue publisher
    | CAS
    v
Protected branch
```

静态契约进入 review 边界；运行 state ref 不授权产品代码；本地 cache 不
授权远端发布；CI success 不授权未批准 criterion。

## 威胁与控制

### Agent 冒充审批者

威胁：Agent 写入 `approved_by: integration-owner` 并把提案当成已批准。

控制：

- approval 命令从认证上下文解析身份，不接受 `--by` 自由文本；
- Plan digest 与审批事件独立记录；
- 项目所有者交互确认是状态转换前置条件；
- 测试 fixture 使用隔离 identity provider。

### 任务修改未拥有路径

威胁：TaskSpec 声明窄路径，但提交包含其他模块或共享合同。

控制：

- 检查完整 base-to-head commit history，不只最终 diff；
- owned-path glob 采用 fail-closed overlap；
- shared contracts 自动提升为 integration-owner review；
- submission 重新计算 changed paths。

### Review 后偷换 payload

威胁：review 通过后修改 branch、测试或生成文件。

控制：

- immutable submission 绑定 source range 和 aggregate patch；
- candidate payload 必须与 submission patch 等价；
- submission ref 禁止重写；
- 新 payload 必须产生新 attempt 和 review。

### 伪造 TDD

威胁：

- 实现先于测试提交；
- 生产代码加入后回滚再声明 Red；
- 用 compiler error、timeout 或 skip 冒充 Red；
- Red 后弱化 oracle；
- 编写不会验证 criterion 的测试。

控制：

- 扫描 base-to-Red 每个 commit；
- 分类基础设施失败与预期断言；
- frozen oracle blob 和 failure fingerprint；
- Plan criterion 与 focused Gate 显式映射；
- high/critical Red 需要独立 reviewer；
- mutation/adversarial fixture 验证测试有效性。

### Gate 命令注入

威胁：任务在 record 或 TaskSpec 中写入任意 shell。

控制：

- TaskSpec 只引用 Gate ID；
- Gate command 使用仓库拥有的 argv 数组；
- executor 不经 `bash -lc`；
- policy blob 进入 cache 和 attestation digest；
- 未知 Gate 和命令漂移 fail closed。

### 缓存污染

威胁：复用旧 source、错误平台、skip 或不完整执行结果。

控制：

- 第一版 key 绑定完整 source tree；
- 绑定 Gate、policy、toolchain、environment、platform 和 isolation；
- 只缓存 success/no-skip；
- artifact digest 二次校验；
- remote requirement 不能由 local cache 满足；
- nightly 定期强制 cache bypass。

### CI 身份伪造

威胁：同名 workflow/status 或旧 run 被用作候选证据。

控制：

- 绑定 repository、workflow path、workflow blob、run ID、attempt 和 head SHA；
- 检查 required jobs 和 matrix 展开；
- artifact 必须绑定同一 run 和 source SHA；
- skipped、neutral、cancelled 和 partial matrix 拒绝。

### Queue 越权发布

威胁：Queue worker 发布未经 review 的 commit 或覆盖 protected branch。

控制：

- worker 只能发布由 valid submission 构造的 candidate；
- candidate parent 必须等于 protected head；
- push 使用 force-with-lease/CAS，禁止 force；
- branch protection 对 worker 同样生效；
- acceptance attestation 在 push 前生成，push 后复核；
- worker credential 不具有 Plan approval 权限。

### Speculative train 污染

威胁：前序失败后发布包含失败 payload 的后序 candidate。

控制：

- candidate manifest 记录完整前缀；
- 发布必须按 train 顺序；
- 后序 candidate parent 必须已经成为 protected head；
- 任一前序失败使其后代 candidate 不可发布；
- 后续 payload 从最后 accepted base 新建 train。

### 状态与产品分支不一致

威胁：attestation、state event 和 protected branch 部分成功。

控制：

- `done` 是三方派生结果，不依赖单字段；
- 所有 state 更新使用 append-only event 和 CAS；
- recovery 可从 protected branch 与 attestation 重建最终事件；
- attestation 创建但未发布不会产生 `done`；
- 发布后恢复不回退 accepted branch。

### 工作日志泄密

威胁：命令输出、环境变量、token 或用户内容进入 timing/event/artifact。

控制：

- event 只保存 Gate ID、digest、duration 和 outcome；
- 不保存 command stdout，失败日志单独受限保留；
- URL 和 credential 经过脱敏；
- artifact 设置大小、路径、文件数和保留期上限；
- 禁止上传 `.git`, home 和环境快照。

## 可用性威胁

- GitHub 不可用：保持 `queued`，不降级为本地接受；
- CI 配额不足：限制 train 深度，不跳过 required Gate；
- state ref 锁遗留：通过 actor lease 和进程/时间证据恢复；
- cache 损坏：删除重算，不阻塞状态恢复；
- queue worker 崩溃：从 refs 派生待处理队列；
- worktree 遗留：不改变远端 state，清理前验证 dirty 状态。

## 范围外

以下问题不由 V2 单独解决，但必须记录：

- GitHub 平台或 runner 基础设施完全失陷；
- 项目所有者凭据被盗；
- 编译器、Action 或依赖供应链的未知后门；
- 恶意 reviewer 与恶意项目所有者合谋；
- 产品运行时自身的网络和文件安全。

这些风险由最小权限、Action SHA pinning、依赖 pinning、密钥轮换、
SBOM/provenance 和独立安全审查补充。

## 安全验收样例

V2 上线测试必须至少覆盖：

- Agent 不能自行批准 Plan；
- task-authored command 被拒绝；
- 修改过的 submission ref 被拒绝；
- stale/wrong-workflow CI 被拒绝；
- skipped matrix 被拒绝；
- cache key 任一字段变化都 miss；
- 前序失败的 train 后代不能发布；
- CAS 竞态只有一个 publisher 成功；
- 发布成功但 state 写失败可恢复；
- acceptance attestation 不要求 candidate 预知自身 SHA。
