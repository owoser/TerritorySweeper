# TerritorySweeper

> 基岩版 BDS 原生插件：在自己的 PLand 领地内，按层、按分块推进的方块清理工具。
> 基于 **LeviLamina (LL3)** + **PLand** + **DebugShape**，目标平台 Windows x64 / Release。

---

## 1. 功能概览

| 功能 | 说明 |
| :--- | :--- |
| 领地内清扫 | 只校验**玩家脚下坐标**所在领地，选区点再逐个校验是否落在领地 AABB 内 |
| 权限 | 仅领地主人（Owner）/ 领地管理（Admin）/ PLand 全局管理员可用；Member 与访客一律拒绝 |
| 分层推进 | 每层先倒计时（默认 10 秒）再清理，BossBar 实时显示倒计时与整体进度 |
| 分块清理 | 面积超过阈值时按 10×10 矩阵分批处理，块间有间隔，边缘残差自动裁剪 |
| 双经济后端 | 默认 LLMoney，可切换为计分板（Scoreboard） |
| 挂起账单 | 未付款方案按领地保存并可跨重启恢复；再次回到领地木棍点击即可继续支付 |
| 按比例退款 | 终止任务时按未完成工作量比例退还已付费用（手动终止与暂停超时自动终止同样适用），退款写入审计表 |
| SQLite 持久化 | 任务、账单、退款审计存于本地数据库，增量写入，WAL 保证崩溃安全 |
| 视觉反馈 | 选点时显示领地金色边框，选完自动关闭；清理期间常驻显示任务区域青色边框 |
| 看门狗 | 按**领地 AABB**（而非清扫选区）判定领主是否离开，15 秒后自动暂停；回到领地自动续跑；暂停 300 秒自动终止 |
| API 自检 | 启动时逐符号解析依赖插件导出，缺失只降级对应功能并打印报告，不会导致插件加载失败 |

---

## 2. 依赖

| 依赖 | 版本 | 作用 | 是否必需 |
| :--- | :--- | :--- | :--- |
| LeviLamina | 26.51.x | 插件加载器 / SDK | 必需 |
| PLand | 0.25.0+ | 领地查询、角色权限、领地 AABB | 必需 |
| DebugShape | 0.11.0+ | 选区与任务区域线框 | 可选（缺失仅不显示边框） |
| LegacyMoney | 0.22.0+ | 默认经济后端 | `economy_type=LLMoney` 时必需 |

> 本插件**不在链接期**引用 PLand.dll / DebugShape.dll / LegacyMoney.dll 的导入库。
> 所有跨插件调用都在运行时通过 GetProcAddress 解析，因此依赖插件升级后即便某个符号改名，
> 本插件依然能正常加载，只是对应功能降级并在控制台给出明确告警。

---

## 3. 构建

```powershell
# 首次构建前（修复 xmake 3.1.1 与 LeviLamina 仓库的 semver 兼容问题）
powershell -ExecutionPolicy Bypass -File .\tools\repair-xmake-env.ps1

xmake f -y -p windows -a x64 -m release
xmake
```

构建依赖：LeviLamina SDK + **sqlite3**（静态链接，随插件 DLL 分发，不引入额外运行时 DLL 依赖）。

产物：`bin/TerritorySweeper/`（TerritorySweeper.dll + manifest.json + pdb）。

部署：把 `bin/TerritorySweeper/` 整个复制到服务端 `plugins/TerritorySweeper/`。

> semver 问题说明：levimc-repo 中 bdslibrary 等包使用四段式版本号（如 1.21.3.01），
> xmake 3.1.1 无法解析，会让所有 add_requires 报 unable to parse semver。
> `tools/repair-xmake-env.ps1` 会清理该包并归一化 quick_search 缓存。
> 每次执行 `xmake repo -u` 之后需要重新运行一次。

---

## 4. 指令

主命令：`/ts`（权限等级 Any，功能内置领地主人校验）

| 指令 | 说明 |
| :--- | :--- |
| `/ts` | 输出帮助 |
| `/ts new` | 创建任务：校验并发上限与挂起账单，开启领地边框并进入选点 |
| `/ts pos1 [x y z]` | 设置点 A（不带坐标时取脚下方块） |
| `/ts pos2 [x y z]` | 设置点 B（不带坐标时取脚下方块） |
| `/ts ok` | 确认选区并打开方向选择菜单 |
| `/ts list` | 查看进行中任务与待支付账单 |
| `/ts start` | 继续本领地已暂停的任务 |
| `/ts stop` | 暂停本领地任务 |
| `/ts kill` | 终止本领地任务 |

## 5. 木棍交互

手持 `minecraft:stick`：

- **左键方块**：设置点 A（同时阻止真的破坏方块）
- **右键 / 手机点击方块**：第一次设置点 A，第二次设置点 B
- **选点完成后再点**：打开任务菜单

点击时按当前领地状态自动分流：

```
运行中 / 已暂停任务 -> 任务管理面板（暂停/继续、停止、关闭）
待支付账单         -> 账单面板（立即支付 / 保留 / 关闭）
空闲               -> 进入选点流程（先校验并发上限）
```

## 6. 配置

配置文件为 `plugins/TerritorySweeper/config/config.jsonc`，格式是 **JSONC**：

- 后缀用 `.jsonc` 而不是 `.json`，VS Code 等编辑器会按「JSON with Comments」处理，`//` 注释不会报错；
- 注释只描述字段含义、不包含当前取值，字段之间留 2 行空行；
- 插件只读取该文件，除首次生成/迁移外不会覆盖它，你手写的注释与排版都会保留。

> **兼容旧文件**：如果目录里只有 `config.json`，插件会自动把它改名为 `config.jsonc`（内容原样保留）；
> 若该文件还是更早的 `"// 说明": 值` 伪装注释格式，则会重排为新版注释排版，并把原文件备份为
> `config.json.bak-<时间戳>`。两者同名同时存在时优先读取 `.jsonc`，`.json` 会被忽略。

关键项：

```jsonc
// economy_type            LLMoney（默认） / Scoreboard
// scoreboard_objective    计分板模式绑定的目标名（需预先 /scoreboard objectives add <name> dummy "金币"）
// moneyname               货币显示名
// global_task_limit       全服并发任务上限（默认 3）
// player_task_limit       单玩家并发上限（默认 1）
// bossbar_enabled        是否显示 BossBar 进度条（排查进服异常时先关掉）
// region_frame_enabled   是否显示选区/任务区域线框（依赖 DebugShape）
// layer_countdown_seconds 每层倒计时（默认 10）
// chunked_clean_enabled   是否启用分块（默认 true）
// chunk_matrix_width/depth 分块尺寸（默认 10x10）
// chunked_min_area        面积超过该值才分块（默认 1024 = 32x32）
// chunk_interval_seconds  分块间隔（默认 2）
// pause_on_player_leave   离开领地是否暂停（默认 true）
// auto_resume_on_return   回到领地是否自动续跑（默认 true，仅对"因离开暂停"的任务生效）
// leave_check_tolerance   判定是否在领地内时的水平容差（默认 2 格）
// player_leave_pause_seconds 离开多久暂停（默认 15）
// pause_auto_cancel_seconds  暂停多久自动终止（默认 300）
// price_levels[]          阶梯价：按 X/Z 跨度匹配第一个满足的档位
```

### 计费规则

- 选区平面尺寸取 `W = |x1-x2|+1`、`D = |z1-z2|+1`
- 在 price_levels 中按顺序匹配第一个满足 `W <= max_width && D <= max_depth` 的档位
- 总价 = 该档 price_per_layer × 层数
- 超出最大档位时按最大档计费，并在费用表单中给出提示

### 层数计算

- **向下清理**：从玩家脚底层起向下；留空时自动扫描选区内的基岩层，止于基岩之上；手动输入时同样做基岩保护与世界底部钳制
- **向上清理**：从玩家脚底层起向上；留空时自动寻找第一个"整层全空气"的层，止于其下一层
- 始终不破坏 minecraft:bedrock

---

## 7. 代码结构

```
src/
  Plugin.cpp/.h        插件生命周期
  Config.cpp/.h        配置模型与读写
  Compat.cpp/.h        依赖插件运行时符号探针（PLand / DebugShape / LegacyMoney）
  Economy.cpp/.h       双后端经济（LLMoney / Scoreboard）
  Visual.cpp/.h        BossBar（BossEventPacket） + DebugShape 线框
  Selection.cpp/.h     领地上下文 + 选区状态
  Task.cpp/.h          任务模型、调度器、清扫引擎、看门狗、退款计算
  Forms.cpp/.h         表单流转
  Commands.cpp/.h      /ts 指令
  Interaction.cpp/.h   木棍监听、玩家上下线、每秒驱动
  Database.cpp/.h      SQLite 轻量封装（连接、预编译语句、事务）
  TaskStore.cpp/.h     表结构 + 任务/账单/退款审计的 CRUD
  MemoryOperators.cpp  统一内存分配运算符（LL3 强制要求）
```

数据文件：`plugins/TerritorySweeper/data/territorysweeper.db`（SQLite，WAL 模式）

表结构：

| 表 | 说明 |
| :--- | :--- |
| `meta` | 键值元数据（任务自增号等） |
| `tasks` | 进行中 / 已暂停任务（含完整 SweepPlan 展平列与进度游标） |
| `bills` | 待支付账单（`land_id` 为主键，一块领地至多一条） |
| `refunds` | 退款审计（任务号、金额、剩余工作量千分比、原因、时间） |

> WAL 模式会额外生成 `territorysweeper.db-wal` 与 `-shm`，属正常现象；备份请一并复制或先停服。
> 若数据库打开失败，插件降级为「仅内存模式」并输出 error 日志，不影响服务器运行。

---

## 8. 退款规则

终止任务时按**未完成工作量比例**退还已支付费用，适用于三条路径：`/ts kill`、管理面板「停止任务」、暂停超时自动终止。

```
totalUnits = layerCount × chunksPerLayer()   // 未分块时 chunksPerLayer = 1
doneUnits  = layersDone × chunksPerLayer() + chunkIndex
退款       = paidAmount × (totalUnits - doneUnits) / totalUnits   // 向下取整
```

- 基准是**实际扣款额 `paidAmount`**，不是按当前配置重算的价格，避免改价/折扣导致多退。
- 非分块任务按「层」粒度退款；分块任务按「层 × 子分块」粒度退款，与真实完成度一致。
- 已开始但未清理完的当前子分块不予退款（它占用了服务器时间片）。
- 退款即时到账（LLMoney 按 XUID / 计分板按玩家名），离线玩家同样可到账。
- 每次退款写入 `refunds` 表；若经济后端不可用，仍会终止任务并输出 error 日志以便人工补偿。
- 任务正常完成（跑到最后一层）不产生退款。

示例（已付 100，共 10 层、未分块）：进度 0 层退 100；4 层退 60；9 层退 10；10 层退 0。

---

## 9. 看门狗判定规则

`pause_on_player_leave` 打开时，每秒对每个运行中任务执行一次判定（`evaluateLeaveState`）：

1. 领主维度与任务不同 → **离开**；
2. 优先用任务里**固化的领地 AABB**（创建任务时写入，schema v2 持久化）做包含判定；
2. 用 `LandRegistry::getLand(landId)` 取任务所属领地的 **AABB**（不是清扫选区），
   水平带 `leave_check_tolerance` 容差、垂直带少量上下容差做包含判定；
3. AABB 判定为外时，再用 `getLandAt(脚下坐标)` 复核，id 相同即视为**在内**（覆盖子领地与边界误差）；
4. 领地已删除 / PLand 不可用 → **无法判定**，按"在内"处理，绝不误暂停。

领主实体连续 2 秒找不到才视为离线（去抖）。判定为离开后累计到 `player_leave_pause_seconds` 才暂停，
并标记为"因离开暂停"；领主回到领地且 `auto_resume_on_return` 为真时自动续跑。手动暂停的任务不受影响。

`debug: true` 时每秒输出一行：

```
[watchdog] task=#1 owner=Allen13in dim=0 pos=(79.0,71.0,214.0) land=#12 inside=true off=0s
```

## 10. BossBar 发包顺序（重要）

Bedrock 的 BossBar 是**挂在实体上的**，不是"无名进度条"。因此每次刷新必须按
LSE `PlayerClass::setBossBar`（也就是 YEssential/Fcam 用的那个 API）的顺序三连发：

1. `BossEvent(Remove)` —— 清掉同 id 的旧条
2. `AddActorPacket` —— 造一个看不见的 `player` 假实体（位置在维度最低高度 -64），
   `mEntityId = mRuntimeId = bossId`；其余字段留空
3. `BossEvent(Add)` —— `mBossID/mName/mHealthPercent(0~1)/mColor/mOverlay`

`BossBar::hide` 只发 `BossEvent(Remove)`。

> 只发第 3 步（缺 `AddActorPacket`）会让客户端拿着一个指向不存在实体的 boss id 去渲染，
> 表现为客户端报错/掉线。
>
> `AddActorPacket` 的构造函数未导出，必须通过 `MinecraftPackets::createPacket(MinecraftPacketIds::AddActor)`
> 创建（与 LSE 一致）。若运行时该工厂不可用，会跳过 AddActor 并仅发送 BossEvent，不会崩服。

**实现要点（对齐 LL 文档 / LSE）**

- 两个包都通过 `MinecraftPackets::createPacket(MinecraftPacketIds::…)` 创建：
  包头里的 `mSerializationMode` 由 BDS 构造函数初始化，栈上默认构造会留下未定义值，
  写出来的包可能不合规（曾导致客户端 `ClientDisconnection`）。
- 字段直接通过文档公开的成员访问（`mBossID / mEventType / mName / mHealthPercent / mColor / mOverlay`），
  不再手动 `static_cast` 到 payload 结构。
- 创建后校验 `pkt->getId()` 与期望一致，避免类型不符时强转。

**刷新策略**：`bossbar_update_mode`

| 值 | 行为 | 包量 |
| :--- | :--- | :--- |
| `incremental`（默认） | 首次 Add，之后只发变化的 `UpdateName` / `UpdatePercent` / `UpdateStyle` | 1~2 包/秒 |
| `recreate` | 每秒 Remove → AddActor → Add 完整重建（与 LSE 一致） | 3 包/秒 |

**排查开关**：`bossbar_enabled` / `region_frame_enabled`，任一关闭即可隔离显示层；
`debug: true` 打开后会输出每个 `[bossbar]` 发包与 `[task]` 推进日志。

## 11. 已知取舍

1. **长加载**：未使用 BDS TickingArea（其 _addArea 需要 LevelStorage，且会与玩家自建区域混淆），
   改为在清扫过程中按区块触达保持区块已加载状态，避免玩家离开导致区块卸载。
2. **自动层数扫描**：选区较大时逐列扫描成本较高，扫描在表单提交时一次性执行；
   极端大选区建议手动输入层数。
