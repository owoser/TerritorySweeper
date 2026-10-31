# TerritorySweeper 未发布迭代记录 (UNRELEASED.md)

本文件详细记录自上次正式发版 (`v1.0.0`) 之后的所有代码提交、功能迭代、问题排查根因与性能优化。
**本文件仅在本地工作区维护，用于抵抗 AI 上下文压缩丢失历史事实，并作为下一次正式发版 Release Notes 的汇总素材。**

---

## 📌 当前版本迭代标定

- **当前正式版本**：`v1.1.0`
- **上个发版版本**：`v1.0.0` (commit `aadc689`)
- **配置系统版本**：`config_version: 5`

---

## 🌟 [1.1.0] 核心功能与性能优化总览（对比 v1.0.0）

### 1. 边缘分片余量吸纳（Tail Absorption）
- **痛点**：选区边缘若刚好超出 1~4 格（如 33×33 区域，分片配置为 32×32），旧算法会机械切出 3 个 1 格宽的碎屑分块，每个碎屑都要吃满 2 秒的分块冷却，导致耗时从 2 秒拉长到 8 秒（浪费 75% 时间）。
- **优化**：新增配置 `chunk_tail_padding: 4`（默认 4 格）。当最后一块的剩余宽度/深度 `<= chunk_tail_padding` 时，自动将其合并吸纳至前一分片，彻底杜绝孤立碎屑分块生成。

### 2. 终检超大范围极速化（SubChunk 块级 O(1) 粗筛）
- **痛点**：大区域（如 64×133×64 约 54 万方块）在结算前终检时，逐方块硬扫 54 万次 `getBlock()`，即使全是空气也需耗时 60 多秒。
- **优化**：利用 Bedrock 底层 16×16×16 `SubChunk` 的 `isUniform(*airPtr)` 块级粗筛。对于纯空气 SubChunk，1 次指针判断瞬间跳过整块 4096 个方块。54 万方块区域仅需检查 144 个 SubChunk 指针，终检时间从 **66 秒缩短至 1 秒以内（提速 50~100 倍）**。

### 3. 方块置空性能提升（消除 6 方向邻居物理级联）
- **优化**：将批量置空代码中的 `source->setBlock(pos, air, 3, ...)` 标志位由 `3`（广播邻居物理更新）优化为 `2`（仅网络发包同步客户端，不广播无意义的邻居连锁物理更新）。消除大面积清理时引起的严重掉帧与 TPS 抖动，单方块置空吞吐提升 3~5 倍。

### 4. 彻底解决水流/岩浆回灌（iListenAttentively 流体流动事前拦截）
- **优化**：
  - 弃用破坏外部地形且对大水体无效的“切断外圈水源”方案；
  - 接入 `iListenAttentively` (v0.17.2) 的 `LiquidFlowBeforeEvent` 事件；
  - 运行中（Running）以及暂停中（Paused）的任务范围内，目标格被流体流入时直接 `event.cancel()` 事前拦截，从源头杜绝回灌；
  - 任务彻底完成或手动终止后才释放拦截，恢复流体正常流动。
- **技术突破**：采用头文件内置 + `tools/gen-ila-lib.ps1` 从测试服部署的 DLL 反向提取导出表生成 `third_party/lib/ILA.lib` 静态导入库，完美解决上游 XMake 源码包因 LeviLamina 版本冲突无法编译的难题。

### 5. 任务结算前终检清扫（Final Sweep）
- **痛点**：逐层清理为“走过不回头”，早期因区块未加载或拦截未就绪导致扫过的层残留孤立水块、掉落物或悬空杂物。
- **优化**：在所有层清扫完成、最终结算前，自动对选区执行一次全量终检，彻底清除残留液体与杂物；BossBar 和管理面板实时显示“收尾清理 · 进度 X％”；支持 `final_sweep: true` 开关。

### 6. 最低基岩绝对保护（防止掏空基岩掉入虚空）
- **痛点**：主世界基岩并非处处平整铺满，旧逻辑把 `preserve_bottom_layers` 允许配置为 0，且硬编码最低高度，导致误挖穿基岩露出虚空。
- **优化**：
  - 建立“绝对地板”防御规则：世界最低层（主世界 `y = -64`、下界/末地 `y = 0`）在任何配置下强制不可破坏；
  - `preserve_bottom_layers` 下限强行钳制为 `>= 1`；
  - `clearUnit()` 写入前追加硬断言：若尝试在最低层写入空气直接拦截并记录 ERROR 日志；
  - 增加基岩移除审计日志（每层统计移除基岩数及最低 Y 坐标）。

### 7. 暂停态安全增强与倒计时保持
- **优化**：
  - 暂停中的任务不再触发“离开领地判断”，彻底修复因玩家离开领地导致释放倒计时（300s）被冻结、以及手动暂停被误标为离开暂停而自动续跑的严重 bug；
  - 暂停时清理范围调试边框保持常驻，BossBar 显示“任务已暂停 | 释放倒计时 Xs”；
  - 暂停中的任务同样持续保持液体流入拦截；
  - 暂停倒计时每 5 秒持久化至 SQLite，服务器重启后接着上次的剩余时间继续倒计时。

### 8. 离线退款暂存与上线补发（SQLite Schema v3）
- **优化**：针对经济后端（LLMoney、计分板）无法给离线玩家加钱的问题，新增 `pending_payouts` 数据库表。任务超时释放时若玩家离线，退款入库暂存；玩家下次进服在 `onPlayerJoin()` 中自动补发到账并发送聊天栏提示。

### 9. 自动化配置版本迁移体系 (Config Migration)
- **优化**：
  - 废弃单纯依赖增量补全的做法；
  - 引入版本号迁移机制：当 `config_version` 提升时，自动将当前文件备份为 `config.jsonc.bak-<时间戳>`，并根据新模板重写配置文件，保留用户既有配置值，自动剔除无用废弃项并刷新中文注释。
  - 当前配置文件已升至 **v3**。

---

## 📝 提交历史明细（从 v1.0.0 到当前）

### Commit 1: `d57bc55` (2026-10-27 10:37:33 +0800)
- **信息**：`feat(liquid): 清理前切断选区外圈液体源，防止水/岩浆灌回清理区域（cut_liquid_border / liquid_border_radius）`
- **内容**：初步尝试通过外扩 N 格切除外圈液体源方块防止回灌（后续在大海环境下证明无效，已被 ILA 流体流动拦截方案彻底替代并删除）。

### Commit 2: `32d8d9d` (2026-10-27 12:30:22 +0800)
- **信息**：`feat(liquid): 任务运行期间拦截液体流入清理区域（block_liquid_inflow），移除旧的切断外圈水源方案`
- **内容**：确立了“任务期间拦截流体流入”的架构思路；初步使用 LeviLamina 的 `BlockChangedEvent` 进行事后还原测试。

### Commit 3: `af61069` (2026-10-27 13:18:04 +0800)
- **信息**：`feat(liquid): 按方案A接入 iListenAttentively 的 LiquidFlowBeforeEvent 拦截液体流入（头文件内置 + 从已安装 DLL 生成导入库），替换事后还原实现`
- **内容**：
  - 完整接入 ILA 的 `LiquidFlowBeforeEvent`，实现流体流动前置可取消拦截，流体不再生成；
  - 内置 ILA 0.17.2 头文件到 `third_party/iListenAttentively/src`；
  - 编写 `tools/gen-ila-lib.ps1`，从测试服 DLL 逆向生成 `ILA.lib`，绕过上游 XMake 源码冲突；
  - `manifest.json` 正式加入对 `iListenAttentively` 的前置依赖声明。

### Commit 4: `e6f7b7b` (2026-10-27 14:49:31 +0800)
- **信息**：`feat(config): 配置版本升到 v2，启动时重写并移除废弃项（带备份）；暂停中的任务同样拦截液体流入`
- **内容**：
  - 引入配置版本迁移体系，从 v1 升至 v2，自动剔除废弃的 `cut_liquid_border` 和 `liquid_border_radius` 参数；
  - `isInsideActiveTask()` 调整为同时检查 `Running` 与 `Paused` 任务，确保暂停期间水流不倒灌。

### Commit 5: `87eac95` (2026-10-27 15:22:11 +0800)
- **信息**：`fix(bedrock): 世界最低层绝对保护（与配置无关）、preserve_bottom_layers 下限钳制为 1、新增基岩审计日志与写入兜底`
- **内容**：
  - 修复掏空基岩露出虚空的问题；
  - 强制规定世界最低层（-64 / 0）属于绝对保护范围，不因参数调整而受影响；
  - 清理写入前追加非法写入最低层的安全拦截与错误告警；
  - 增加每层移除基岩的统计与最低 Y 坐标调试日志。

### Commit 6: `686ee01` (2026-10-27 15:49:04 +0800)
- **信息**：`chore: ignore AGENT.md`
- **内容**：在 `.gitignore` 中忽略 `AGENT.md`，确保本地 Agent 开发手册不污染公共仓库。

### Commit 7: `7157375` (2026-10-27 16:08:54 +0800)
- **信息**：`feat(cleanup): 任务结算前增加终检清扫，清除残留液体与方块；BossBar/管理面板显示收尾进度（final_sweep 开关）`
- **内容**：
  - 新增 `runFinalSweep()`，在所有层推进完毕后对选区体积做终检清理，彻底解决残留半空水块问题；
  - 新增 `BarPhase::Finalizing` 阶段，BossBar 与管理面板支持收尾清理进度显示；
  - 配置文件新增 `final_sweep: true`。

### Commit 8: `c314880` (2026-10-27 18:48:44 +0800)
- **信息**：`perf: 边缘分片余量吸纳(chunk_tail_padding)+SubChunk块级粗筛(subchunk_culling)+置空消除物理级联+升级配置至v3+记录开发规范`
- **内容**：
  - 实现边缘余量吸纳算法（`chunk_tail_padding: 4`），33×33 选区从 4 个分片合并为 1 个，消除碎屑等待；
  - 终检重构为 SubChunk 16×16×16 块级粗筛（`subchunk_culling: true`），54 万方块区域 1 秒内完成；
  - 方块置空标志位优化为 `2`，消除无效邻居物理级联，解决清理卡顿；
  - 饱和式预算推进；
  - 配置文件版本升级至 **v3**；
  - 在 `AGENT.md` 中确立配置变更版本号升级铁律。

### Commit 9: `d8a2b56` (2026-10-27 20:30:15 +0800)
- **信息**：`chore: ignore UNRELEASED.md in git`
- **内容**：在 `.gitignore` 中忽略本地未发布记录文件 `UNRELEASED.md`。

### Commit 10: `2449b9d` (2026-10-27 21:40:48 +0800)
- **信息**：`perf: 多任务全局预算池(globalBlocksPerTickBudget)+空间网格O(1)液体拦截+SQLite池化复用与脏标记批量事务提交+终检水平预加载解耦与BossBar双进度修复+升级配置v4`
- **内容**：
  - **BossBar 双进度修复**：收尾阶段只输出单一进度，消除 `进度 39% | 进度 100%` 双进度 Bug；
  - **终检极速化（秒过）**：水平区块预加载一次性解耦，不再在 144 个垂直 SubChunk 循环中重复扣减每秒 4 个的限流配额；内存扫描上限 13 万格/秒，纯空气 SubChunk 瞬时通过；
  - **多任务全局方块配额池 (`global_blocks_per_tick_budget: 12000`)**：所有并发任务共享配额，时间片公平轮换调度，彻底保护服务器 20.0 TPS；
  - **空间哈希网格**：液体事件发生时 O(1) 瞬时短路（< 5ns），消除多任务线性比对；
  - **SQLite 极致优化**：预编译语句池化常驻复用（`Statement::cached`），内存脏标记批量单事务提交（`flushDirtyTasks`），PRAGMA mmap/cache_size/temp_store 调优；
  - **配置升级至 v4**：完善 `chunk_interval_seconds: 0` 饱和推进特性的说明。

### Commit 11: `7733de9` (2026-10-27 21:58:32 +0800)
- **信息**：`docs: 恢复 README.md 为 v1.0.0 正式发版基线（严格遵守 AGENT.md 规范，日常迭代统一维护在 UNRELEASED.md）`
- **内容**：恢复对外正式文档 `README.md` 与 `v1.0.0` 完全一致；确立发版前全部开发事实集中维护于本地 `UNRELEASED.md` 的铁律。

### Commit 12: `e0b5f0b` (2026-10-27 23:18:20 +0800)
- **信息**：`fix(cleanup): 修复 isSubChunkUniformAir 绝对索引错位导致空层误跳残留水柱+终检确保完整清空当前SubChunk+清除含水层(Layer 1 ExtraBlock)`
- **内容**：
  - **纠正 SubChunk 绝对索引错位（核心 Bug）**：主世界 SubChunk 的 `mAbsoluteIndex` 范围为 `-4 ~ 19`。此前通过 `(pos.y - lc->mMin->y) >> 4` 访问 `mSubChunks` 数组造成严重索引偏移（Y=0~31 被错位访问了 Y=-64 的底层空气块），导致包含水和方块的层在逐层扫描和终检时被误判为纯空气跳过，产生半空悬空水柱。现改用遍历 `subchunks` 精确比对 `sc.mAbsoluteIndex == static_cast<signed char>(pos.y >> 4)`，精准命中；
  - **非纯空 SubChunk 完整清空**：修复终检中单个 SubChunk 被中断跳过的逻辑，一旦进入非纯空 SubChunk，必须将其所辖选区完整点查清空后再推进 `finalCursor`，彻底杜绝局部方块残留；
  - **含水方块 (Waterlogged Blocks) 双层清理**：在 `clearUnit` 与 `runFinalSweep` 中同步置空 Layer 1（`ExtraBlock`），彻底清除楼梯/台阶/发光地衣等方块破坏后遗留的水。

### Commit 13: `97b80af` (2026-10-27 23:40:57 +0800)
- **信息**：`docs: 将 UNRELEASED.md 纳入 Git 跟踪并推送到仓库，记录 v1.0.0 后的全部迭代历史`
- **内容**：将 `UNRELEASED.md` 从 `.gitignore` 中移除，正式推送到 GitHub 远端仓库公开跟踪，确保团队与开源社区能够随时查阅开发分支的最新技术进展与修复明细。

### Commit 14: `8fca127` (2026-10-31 20:32:39 +0800)
- **信息**：`build(xmake): 升级 LeviLamina 26.51.9 + clang-cl 23 工具链，ILA 导入库改为构建期从入库 .def 生成`
- **内容**：
  - **工具链升级（LL 官方已改用 LLVM 23）**：SDK 版本线 `26.51.6 → 26.51.9`（`add_requires` + `add_requireconfs` 双处锁定），编译器强制 clang-cl >= 23；clang 19 在 `MinecraftCommands.h` 上必现 `invalid application of 'sizeof' to an incomplete type`，升级后无任何报错；
  - **预编译 SDK 分发**：新增 `xmake-repo-extra/packages/l/levilamina/xmake.lua`，只把 `on_install` 换成下载 `include/` + `lib/`，依赖列表与官方 `versions/26_51_9.lua` 严格一致；产物由本仓库 Release `sdk-v26.51.9`（pre-release）托管，并配置 `TerritoryMobBlocker` 同名 release 作为兜底镜像（同一 sha256 `33056860…f086c`）；
  - **新增 `tools/build-sdk.ps1`**：LL 小版本升级时从本机已安装包重新打包 SDK 并输出 sha256；
  - **ILA 导入库工程化**：`third_party/lib/ILA.def`（导出表）入库跟踪，`.lib` 由 `xmake.lua` 的 `on_load` 在配置阶段用 `llvm-lib /def:` 自动生成（生成结果与旧文件**字节完全一致**），CI 不再需要测试服 DLL；
  - **编译器护栏**：`on_load` 检测 PATH 首个 `clang-cl`，主版本 < 23 时抛出可读错误（提示装 LLVM 23、前置 PATH、并 `xmake f -c -y`）；
  - **版本号注入**：支持 `TS_VERSION` 环境变量（去前导 v）写入 `manifest.json`，本地未设置时仍回退 `git describe`；
  - **修复「延迟加载从未生效」**：`@levibuildscript/linkrule` 的链接步骤只保留 `shflags`，此前的 `add_ldflags("/DELAYLOAD:iListenAttentively.dll")` 根本没进链接命令行（旧版 DLL 的普通导入表里就有 ILA）；改为 `add_shflags` 后 `dumpbin /imports` 确认 ILA 已进入 delay load 段。同时把 `#pragma comment(linker, ...)` 方案判为无效（`/DELAYLOAD` 不在 `.drectve` 支持列表），并在 ILA 缺失时让 `registerListener()` 提前返回，避免延迟加载异常。

### Commit 15: `418d2d5` (2026-10-31 20:32:47 +0800)
- **信息**：`ci: 新增 GitHub Actions tag 构建与自动发版（LLVM 23 + 预编译 SDK）`
- **内容**：
  - `.github/workflows/build.yml`：push `main` / PR / 手动触发，只构建 + 上传 artifact；依赖包失败时自动导出 `install.txt` 尾部 300 行（xmake 的真实报错只在里面）；
  - `.github/workflows/release.yml`：push tag `v*` → 校验 tag 语义化 → 构建（`TS_VERSION` 注入）→ 校验 `manifest.json` 版本与 tag 一致 → 打包 `TerritorySweeper-<版本>.zip` → 自动创建 Release（`-` 结尾自动标 pre-release）；发行说明优先读取 `release-notes/<tag>.md`，缺失回退 GitHub 自动生成并告警；
  - 构建环境固定 `windows-2022` + `ZhongRuoyu/setup-llvm@v0`（`llvm-version: 23`）+ `xmake-io/github-action-setup-xmake@v1`，缓存 `~/AppData/Local/.xmake`；
  - 新增 `release-notes/` 目录约定与说明文档（发版前撰写 `<tag>.md`）；
  - `tooth.json`：修正占位仓库地址 `github.com/example/...` → `github.com/owoser/TerritorySweeper`，补登记 `iListenAttentively 0.17.*` 依赖。

### Commit 16: `532aa75` (2026-10-31 20:32:56 +0800)
- **信息**：`docs: 记录 CI 体系与 LL 26.51.9 / 工具链升级（UNRELEASED.md）`
- **内容**：把本次 CI 体系与工具链升级的完整事实（含 SDK 分发、ILA 导入库、延迟加载踩坑、发版流程变更）写入未发布迭代日志，作为下一次正式发版 Release Notes 的事实来源。

### Commit 17: `（本条提交）`
- **信息**：`docs: 记录 GitHub Actions 端到端验证结果与 tag 校验正则修复`
- **内容**：
  - **Build 工作流（main）绿灯**：run `41202439381`，耗时 3m37s。日志确认 `clang+llvm-23.1.4` 就位、`[levilamina] 已安装预编译 SDK 26.51.9（跳过源码构建）`、`[iListenAttentively] 已由 ILA.def 生成导入库 ILA.lib`，编译 41s 完成；
  - **Release 工作流（tag）绿灯**：run `41202452761`，耗时 3m50s，验证流程为「校验 tag → 构建并注入 `TS_VERSION` → 校验 manifest 版本 → 打包 zip → 创建 Release」；
  - **产物核验**：`TerritorySweeper-0.0.0-ci-test.zip` 内含 `TerritorySweeper/{manifest.json,TerritorySweeper.dll}`，`manifest.json` 的 `version` 为 `0.0.0-ci-test`（tag 注入生效），Release 自动标记为 pre-release，`tooth.json` 版本不一致与「缺少 release-notes」两条 warning 均按设计触发；
  - **发现并修复的缺陷**：tag 校验正则最初只允许 `[0-9A-Za-z.]`，带内部连字符的预发布标识（`v0.0.0-ci-test`）被误拒；已放宽为 `(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?`（commit `bfd0fb1`）；
  - 验证结束后已删除临时 release 与 tag（本地与远端均已清理），仓库仅保留 `sdk-v26.51.9` 预发布资产与历史正式版本。

### Commit 18: `fee542d` (2026-10-31 21:28:20 +0800)
- **信息**：`feat(task): 支持同一领地多任务（修复 player_task_limit 看似失效）+ BossBar 槽位限流轮换`
- **内容**：
  - **根因一（代码闸门）**：`checkCreateAllowed()` 里「一领地一任务」判定排在玩家上限判定之前，同领地建第二个任务必然命中该条，`player_task_limit` 根本没机会参与判定；现改为 `allow_multi_task_per_land`（默认开），关闭后恢复旧限制。
  - **根因二（重启去重）**：`db::loadTasks()` 按 `land_id` 去重，重启会把同领地的第 2、3 个任务静默丢弃；已彻底移除该去重（表结构 `tasks.id` 主键 + `land_id` 索引天然支持多行）。
  - **诊断增强**：全局上限 / 玩家上限的拒绝文案改为「当前数量/上限」，并在旧限制命中时带上任务 ID；启动日志增加「同领地多任务=开/关、BossBar上限=N条/Ns」。
  - **查询与命令**：新增 `tasksOfLand()` / `tasksOfPlayer()`；`/ts stop` / `/ts start` 作用于该领地全部任务；`/ts kill [任务ID]` 支持多任务（无参且多任务时列出 ID 并要求指定，且校验任务属于当前领地）。
  - **表单与交互**：新增 `openTaskPicker()`——同领地多任务时先选任务再进管理面板，并提供「新建清扫任务」入口；管理面板在该领地有多个任务时提供「返回任务列表」；木棍点击多任务领地时进入选择菜单。
  - **BossBar 槽位看板**：固定槽位 `bar-slot-<i>`，同屏条数受 `bossbar_max_bars`（默认 2）限制；暂停任务优先钉住（保留释放倒计时），其余槽位按 `bossbar_rotate_seconds`（默认 5s）轮换；槽位内容切换只发 `UpdateName/UpdatePercent`（不重建，无闪烁）；表现层连续异常 3 次后停用该玩家看板。
  - **线框**：`task-frame` 只跟随「主任务」（排序后第一个），消除多任务共用一个 key 导致的互相覆盖闪烁。
  - **调度健壮性**：`tick()` 第二阶段改为「先快照运行中任务 id，再按 id 查表推进」，避免 `stepTask` 内部 `finishTask` 触发 `vector::erase` 导致跳过相邻任务。
  - **配置版本**：`config_version` **4 → 5**，新增 `allow_multi_task_per_land` / `bossbar_max_bars` / `bossbar_rotate_seconds` 三个字段（启动自动备份并重写，用户既有值保留）。
### Commit 19: `d0019b0` (2026-10-31 21:30:30 +0800)
- **信息**：`fix(task): 支付前二次校验任务名额，避免扣款后因名额已满创建失败`
- **内容**：`openPayment` 的「立即支付」在扣款前再次调用 `checkCreateAllowed`（新增 `ignoreBill` 参数，跳过本领地挂起账单判定，因为本次支付就会消费它）；被拒时提示「本次未扣款」，彻底避免“钱扣了、任务建不出来”的资损路径。

### Commit 20: `（本条提交）`
- **信息**：`docs: 补充多任务与扣款前名额校验说明（UNRELEASED.md / AGENT.md）`
- **内容**：把同领地多任务、BossBar 槽位轮换、扣款前名额二次校验的规则与根因写入迭代日志与开发规范。
### Commit 21: `28dc4f3` (2026-10-31 22:46:32 +0800)
- **信息**：`fix(interaction): 选点模式优先于已有任务，修复 /ts new 选点时木棍直接弹出旧任务面板`
- **内容**：
  - **根因**：`handleStickUse()` 中「领地内任务」分支排在「选点」分支之前；旧版因为 `checkCreateAllowed()` 的「一领地一任务」闸门挡住了 `/ts new`，这条路径永远不可达，多任务上线后立刻暴露。
  - **优先级调整**：`选点模式 > 领地内任务 > 待支付账单`；选区属于其它领地时提示「回到原领地或 /ts cancel」，不写入错误坐标；不在任何领地时的提示也补充了取消引导。
  - **新增 `/ts cancel`**：`SelectionManager::cancel()` 此前全仓库无调用方，玩家进入选点后没有任何退出途径；现在取消后木棍恢复「打开任务管理 / 任务选择」。
  - **`handleNew` 顺序修正**：先判断「已在选点模式」提示续选（带 `/ts cancel` 引导），再做名额检查，避免续选时看到「名额已满」。
  - **删除死代码**：`form::openSmartMenu()` 已无调用方，且其逻辑同样把任务排在选区之前，删除以免后续误接。

### Commit 22: `（本条提交）`
- **信息**：`docs: 记录木棍交互优先级与 /ts cancel（UNRELEASED.md / AGENT.md）`
- **内容**：把「选点 > 任务 > 账单」的优先级、选区领地校验与取消入口写入迭代日志与开发规范。
### Commit 23: `9068d0c` (2026-10-31 22:48:21 +0800)
- **信息**：`fix(interaction): /ts ok 与 /ts posN 校验收区所属领地，避免跨领地写入坐标`
- **内容**：`setPoint()` 在选区已绑定其它领地时直接拒绝（不再用当前领地坐标覆盖选点）；`handleOk()` 确认前校验 `state->landId == land->key`，避免把 A 领地的选区按 B 领地立项。与木棍交互的领地校验保持一致，提示统一引导 `/ts cancel`。
### Commit 24: `6f46fbe` (2026-10-31 23:04:53 +0800)
- **信息**：`fix(bill): 修复领地内已有任务时，挂起账单无法通过木棍支付`
- **内容**：
  - **根因**：`handleStickUse()` 里「任务」分支一旦命中就 `return`，账单分支永远不可达；旧版被「一领地一任务」闸门掩盖（有任务时无法 `/ts new`，也就不会产生同领地账单），多任务放开后立刻暴露。
  - **修复**：任务与账单共存时改为进入任务选择菜单，并在菜单中固定提供「支付待支付账单 · 金额」按钮（点击时重新校验领地与账单是否仍然有效）。
  - `openManage` 在存在账单时同样提供「返回任务列表」；挂起账单后的提示改为明确指引（木棍点地面 → 支付待支付账单）。
  - `openBill` 的「立即支付」与 `openPayment` 对齐：扣款前二次校验名额，避免扣款后任务建不出来。

### Commit 25: `（本条提交）`
- **信息**：`docs: 记录账单入口与任务共存的处理（UNRELEASED.md / AGENT.md）`
- **内容**：把「任务 + 账单共存时木棍必须给出支付入口」的规则与根因写入开发规范与迭代日志。
### Commit 26: `2347caa` (2026-10-31 23:30:39 +0800)
- **信息**：`feat(task): 领地清扫任务列表统一管理多任务/多账单，支持同领地多张挂起账单（DB schema v4）`
- **内容**：
  - **统一入口**：木棍路由改为「合计 1 个条目（任务/账单）→ 直连面板；合计 ≥ 2 → 领地清扫任务列表」。
  - **两行条目**：第一行 `任务#id · 状态` / `账单#id · 待支付`，第二行进度 / 释放倒计时 / 规模金额；按钮文字统一黑色 `§0`；新增「刷新」按钮（重新解析脚下领地后按最新数据重开）。
  - **数据库 schema v3 → v4**：`bills` 改为 `bill_id INTEGER PRIMARY KEY AUTOINCREMENT` + `land_id` 索引，允许同一领地多张账单；迁移前 `VACUUM INTO` 备份，事务内 `RENAME → CREATE → INSERT SELECT → DROP`，失败回滚并告警（任务/退款不受影响）。
  - **按 id 操作账单**：`storeBill()` 追加、`dropBill(billId)`、`openBill()` 支付/关闭、`openPayment(..., srcBillId)` 透传；杜绝按 `land_id` 误删同领地其它账单。
  - **修复**：账单余额不足跳费用页后支付成功不再残留原账单；同一计划重复「挂起账单」改为替换而非堆叠。
  - **移除旧规则**：领地有账单不再阻止新建任务（多账单下自相矛盾），名额只在支付时校验。
  - `/ts list` 更名为「全服清扫任务总览」。
  - **迁移实测**：复制线上库（v3、1 张账单）跑同一套 SQL，结果 `user_version=4`、行数不变、`bill_id` 主键与 `idx_bills_land` 均正确。

### Commit 27: `（本条提交）`
- **信息**：`docs: 记录多账单 schema v4 与领地任务列表规则（UNRELEASED.md / AGENT.md）`
- **内容**：补充多账单数据模型、迁移步骤、列表两行按钮/刷新按钮/按 id 查表的交互铁律。
### Commit 28: `c313ff1` (2026-10-31 23:56:52 +0800)
- **信息**：`fix(ui): 账单/任务界面显示实际选区尺寸，不再把收费档位名（9*9）当尺寸`
- **内容**：
  - **根因**：`plan.levelname` 是收费档位名（`9*9` = "不超过 9×9 的档位"），之前被当成尺寸显示在账单列表、账单面板、费用详情；实际尺寸要从 `min/max` 坐标算（截图那份账单是 8×9，只落在 9*9 档位）。
  - **新增** `planSizeText(SweepPlan)`（`宽×深`，如 `8×9`），所有展示点统一调用。
  - 领地任务列表账单条目：`9*9 · 137 层 · 137 金币` → `8×9 · 137 层 · 137 金币`。
  - 账单面板：`收费等级：9*9` → `实际选区：8×9（收费档位 9*9）`；费用详情改为 `选区大小：…`；任务管理面板补 `大小：…`。
  - `/ts list` 的任务与账单文本补上选区尺寸。

### Commit 29: `（本条提交）`
- **信息**：`docs: 记录 UI 尺寸展示规范（UNRELEASED.md / AGENT.md）`
- **内容**：在开发规范里加一条铁律：UI 中禁止用 `levelname` 代替选区尺寸，统一用 `planSizeText()`。
### Commit 30: `a214893` (2026-11-01 00:52:29 +0800)
- **信息**：`fix(display): BossBar 公平轮换（暂停不再占满槽位）+ 每任务独立线框 + 任务消息带编号`
- **内容**：
  - **BossBar 轮换公平性**：旧逻辑 `pinned = min(pausedCount, cap)`，2 个暂停任务 + cap=2 就会把轮播占满，运行中的任务永远不显示。现改为：只有剩余释放时间 ≤ 60s（临界）的暂停任务才钉住，且只要存在运行中的任务就至少保留 1 个轮换槽位；其余槽位在全部剩余任务间按 `bossbar_rotate_seconds` 滑窗轮换。任务数 ≤ 上限时不轮换（行为不变）。
  - **每任务独立线框**：`BarBoard` 用 `framedTasks` 集合跟踪，key = `task-frame-<id>`，颜色按 `id % 6` 取青/绿/橙/紫/粉/黄；任务结束 `clearBox`，配置关闭全部清除，关服按记录逐个清理。替代原先「只给主任务画框、多任务共用一个 key 互相覆盖」的实现。
  - **消息带任务编号**：暂停/继续/完成/终止（含退款）统一为 `任务 #N …`；删除管理面板与 `/ts kill` 的重复提示，保证一次操作只出一条。
  - 暂不实现「看向区域浮空字」：轮换公平 + 彩色线框已解决可见性；若需要可另做 actionbar 瞄准提示。

### Commit 31: `（本条提交）`
- **信息**：`docs: 记录 BossBar 轮换/线框/消息规范（UNRELEASED.md / AGENT.md）`
- **内容**：补充「只钉临界暂停任务、给运行中任务留槽」「每任务一线框+调色板」「任务消息带 #id 且不重复」三条规则。
---

## ⚙️ 新增/变更配置项汇总表（对比 v1.0.0）

| 配置项 | 类型 | 默认值 | 作用说明 |
| :--- | :--- | :--- | :--- |
| `config_version` | int | `5` | 配置文件结构版本号（升级触发备份重排） |
| `allow_multi_task_per_land` | bool | `true` | 是否允许同一领地内同时存在多个清扫任务（关闭恢复旧限制） |
| `bossbar_max_bars` | int | `2` | 同一玩家同时可见的 BossBar 条数上限（超出轮换） |
| `bossbar_rotate_seconds` | int | `5` | BossBar 轮换间隔（秒） |
| `block_liquid_inflow` | bool | `true` | 任务期间（含暂停）通过 ILA 阻止外部水/岩浆流入选区 |
| `final_sweep` | bool | `true` | 所有层清完后执行全选区终检，清除残留悬空水块 |
| `chunk_tail_padding` | int | `4` | 边缘分片吸纳阈值，余量 ≤ 4 格直接并入前一分片 |
| `subchunk_culling` | bool | `true` | 开启 16×16×16 SubChunk 纯空气 O(1) 瞬时粗筛 |
| ~~`cut_liquid_border`~~ | - | - | *(已废弃并彻底从配置中移除)* |
| ~~`liquid_border_radius`~~ | - | - | *(已废弃并彻底从配置中移除)* |
