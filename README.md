# TerritorySweeper

> 基岩版 BDS 原生插件：在自己的 PLand 领地内，按层、按分块推进的方块清理工具。
> 基于 **LeviLamina (LL3)** + **PLand** + **DebugShape** + **iListenAttentively**，目标平台 Windows x64 / Release。

---

## 1. 功能概览

| 功能 | 说明 |
| :--- | :--- |
| **领地内清扫** | 严格绑定玩家脚下所在领地，选区自动裁剪到领地 AABB 内，禁止越界清理 |
| **权限隔离** | 仅领地主人（Owner）、领地管理（Admin）或管理员可用；访客与成员一律拒绝 |
| **分层推进** | 支持向下/向上逐层推进，每层开始前可设倒计时；BossBar 实时显示层数、分块、倒计时与总进度 |
| **分块清理与余量吸纳** | 面积超过阈值自动分块；边缘残余宽度 ≤ `chunk_tail_padding` 时自动吸纳，杜绝 1 格碎屑分片 |
| **流体流入拦截** | 接入 iListenAttentively，任务运行与暂停期间通过 `LiquidFlowBeforeEvent` 事前拦截流体流入 |
| **极速终检清扫** | 结算前全选区终检清除残留水块/方块；基于 SubChunk 16×16×16 `isUniform(air)` 块级粗筛 |
| **多任务全局调度** | 全服共享每秒方块配额池（`global_blocks_per_tick_budget`），时间片公平轮换，锁定 20.0 TPS |
| **同一领地多任务** | `allow_multi_task_per_land` 开启时一个领地可并行多个任务，受全服/玩家上限约束 |
| **领地清扫任务列表** | 任务与账单合计数 ≥ 2 时，木棍打开统一列表：两行条目（状态 + 进度/规模金额）、刷新按钮、逐条进入管理 |
| **多张挂起账单** | 同一领地可挂多张账单，各自独立支付/关闭；挂起即画出红色区域线框，支付或关闭即消失 |
| **BossBar 槽位轮换** | `bossbar_max_bars` 限制同屏条数；只钉住剩余释放 ≤ 60s 的暂停任务，其余按 `bossbar_rotate_seconds` 滑窗轮换 |
| **每任务独立线框** | `task-frame-<id>`，按任务 ID 取青/绿/橙/紫/粉/黄，任务结束自动消失 |
| **比例退款与离线暂存** | 终止任务按未完成工作量比例退款；玩家离线时写入 `pending_payouts`，下次进服自动补发 |
| **看门狗与自动续跑** | 按领地 AABB 水平范围判断离开，离开 15 秒自动暂停；返回领地自动续跑（手动暂停不续跑）；暂停 300 秒超时释放并退款 |
| **SQLite 极致读写** | 预编译语句池化常驻复用，内存脏标记批量单事务提交，256MB mmap 与 16MB 页缓存 |
| **API 动态解析自检** | 跨插件调用启动时通过符号探针动态解析，依赖缺失时优雅降级并输出报告 |

---

## 2. 依赖

| 依赖 | 版本 | 作用 | 是否必需 |
| :--- | :--- | :--- | :--- |
| **LeviLamina** | 26.51.x | 插件加载器 / 核心 SDK | 必需 |
| **PLand** | 0.25.x | 领地查询、角色权限校验、领地 AABB | 必需 |
| **iListenAttentively** | 0.17.x | 细粒度事件（流体流动事前拦截） | 必需 |
| **DebugShape** | 0.11.x | 选区与任务区域线框渲染 | 可选（缺失仅不显示线框） |
| **LegacyMoney** | 0.22.x | 默认经济后端 | `economy_type=LLMoney` 时必需 |

---

## 3. 构建与部署

**工具链**：xmake 3.x + **clang-cl 23（LLVM 23）**；LeviLamina **26.51.9** 由 `xmake-repo-extra` 以预编译 SDK 分发（无需编译上游本体）。

```powershell
# 首次配置（必须带 --toolchain=clang-cl；换过 PATH / 编译器后需要 -c 重新探测）
xmake f -c -p windows -a x64 -m release --toolchain=clang-cl -y

# 编译
xmake -y
```

- **构建产物**：`bin/TerritorySweeper/`（`TerritorySweeper.dll` + `manifest.json` + `TerritorySweeper.pdb`）；
- **部署方式**：把 `bin/TerritorySweeper/` 里的 `TerritorySweeper.dll` 与 `manifest.json` 复制到服务端 `plugins/TerritorySweeper/`；
- **版本注入**：设置环境变量 `TS_VERSION=v1.2.0` 时会把去前导 v 的版本写进 `manifest.json`，本地未设置时回退 `git describe`；
- **CI**：`.github/workflows/build.yml`（push `main` / PR 编译校验）、`.github/workflows/release.yml`（推送 `v*` tag 自动构建 + 发版，发行说明读取 `release-notes/<tag>.md`）。

---

## 4. 指令列表

主命令：`/ts`（权限等级 Any，内置领地主人权限校验）

| 指令 | 说明 |
| :--- | :--- |
| `/ts` | 显示指令帮助与基本信息 |
| `/ts new` | 发起新任务：校验名额后开启领地边框并进入木棍选点 |
| `/ts pos1 [x y z]` | 设置点 A（缺省坐标时取玩家脚下方块坐标） |
| `/ts pos2 [x y z]` | 设置点 B（缺省坐标时取玩家脚下方块坐标） |
| `/ts ok` | 确认当前选区并打开清扫方向与层数选择菜单 |
| `/ts cancel` | 取消当前选点模式（选点后木棍才会恢复任务/账单入口） |
| `/ts list` | 查看**全服**清扫任务总览与待支付账单 |
| `/ts start` | 继续本领地全部已暂停的清扫任务 |
| `/ts stop` | 手动暂停本领地全部运行中的清扫任务 |
| `/ts kill [任务ID]` | 终止本领地任务并按未完成比例退款；多任务时需指定任务 ID |

---

## 5. 木棍交互

手持 `minecraft:stick` 点击方块。**仅在下列状态下响应，其余情况完全静默**：

1. 已使用 `/ts new` 进入选点流程；
2. 当前领地存在运行中或已暂停的任务；
3. 当前领地存在待支付的挂起账单。

**权限与开关**：`stick_requires_sneak` 开启后必须下蹲潜行点击；左键设置点 A（同时取消破坏事件），右键/移动端点击依次设置点 A、点 B。

**路由优先级**：

| 状态 | 行为 |
| :--- | :--- |
| 选点模式中 | 设置对应端点坐标并刷新选区线框（`/ts cancel` 退出） |
| 任务 + 账单**合计 1 个** | 直接打开对应面板（任务管理 / 账单支付） |
| 任务 + 账单**合计 ≥ 2 个** | 打开【领地清扫任务列表】 |

**领地清扫任务列表**：

```
任务#12 · 进行中         账单#3 · 待支付
进度 52％ · 第 4/10 层   8×9 · 137 层 · 137 金币
```

- 每条目两行：第一行身份 + 状态，第二行进度 / 释放倒计时 / 选区规模与金额；
- 点击任务进入管理面板（暂停、继续、终止退款），点击账单进入支付面板；
- 「刷新」按最新数据重开列表；
- 存在多个条目时，管理面板提供「返回任务列表」。

---

## 6. 配置文件详解 (`config.jsonc`)

配置文件位于 `plugins/TerritorySweeper/config/config.jsonc`，支持标准 `//` 注释。
版本升级（当前 **`config_version: 5`**）时自动备份为 `.bak-时间戳` 并重写纯净模板，**用户既有配置值完整保留**。

```jsonc
{
    // 配置文件版本号，请勿手动修改
    "config_version": 5,

    // 调试模式：输出发包 / 任务推进 / 交互等详细日志
    "debug": false,

    // 经济系统后端：LLMoney / Scoreboard
    "economy_type": "LLMoney",

    // 计分板目标名称（Scoreboard 模式下生效）
    "scoreboard_objective": "money",

    // 界面文案中显示的货币名称
    "moneyname": "金币",

    // 全服同时存在的清理任务数量上限
    "global_task_limit": 3,

    // 单个玩家同时存在的清理任务数量上限
    "player_task_limit": 1,

    // 全服所有并发任务共享的每秒最大方块处理预算
    "global_blocks_per_tick_budget": 12000,

    // 是否显示 BossBar 进度条
    "bossbar_enabled": true,

    // BossBar 刷新方式：incremental（只发变化字段）/ recreate（每秒完整重建）
    "bossbar_update_mode": "incremental",

    // 是否允许同一领地内同时存在多个清扫任务（false = 恢复旧限制）
    "allow_multi_task_per_land": true,

    // 同一玩家同时可见的 BossBar 条数上限（超出的任务轮流展示）
    "bossbar_max_bars": 2,

    // BossBar 轮换间隔（秒）
    "bossbar_rotate_seconds": 5,

    // 是否显示选区与任务区域的线框（依赖 DebugShape）
    "region_frame_enabled": true,

    // 每层开始清理前的倒计时秒数
    "layer_countdown_seconds": 10,

    // 是否启用分块清理
    "chunked_clean_enabled": true,

    // 分块矩阵跨度（方块数）
    "chunk_matrix_width": 10,
    "chunk_matrix_depth": 10,

    // 面积超过该值时才启用分块清理（1024 = 32x32）
    "chunked_min_area": 1024,

    // 相邻分块之间的等待秒数（0 = 单 tick 连续清扫多个分块）
    "chunk_interval_seconds": 2,

    // 领主离开领地范围时是否自动暂停任务
    "pause_on_player_leave": true,

    // 离开领地多少秒后触发暂停
    "player_leave_pause_seconds": 15,

    // 领主返回领地后是否自动继续任务（只对离开暂停生效）
    "auto_resume_on_return": true,

    // 判定是否在领地内时允许的水平越界容差（格）
    "leave_check_tolerance": 2,

    // 暂停多少秒后自动终止任务并退款
    "pause_auto_cancel_seconds": 300,

    // 是否要求下蹲才响应木棍点击
    "stick_requires_sneak": false,

    // 分块清理时是否跳过纯空气分块（不消耗分块间隔）
    "skip_empty_chunks": true,

    // 整层无需清理时是否跳过该层（不消耗每层倒计时）
    "skip_empty_layers": true,

    // 每 tick 用于空区域扫描的方块读取预算
    "empty_scan_budget": 8192,

    // 离开领地判定是否忽略 Y（true = 只按水平范围判断）
    "leave_check_ignore_y": true,

    // 清理时是否主动加载未加载的目标区块
    "force_load_chunks": true,

    // 每秒最多主动加载的区块数
    "chunk_load_budget": 4,

    // 是否连基岩一起清理
    "clear_bedrock": true,

    // 保留世界最底部的基岩层数（强制 >= 1）
    "preserve_bottom_layers": 1,

    // 任务进行中（含暂停）是否阻止液体流入选区
    "block_liquid_inflow": true,

    // 结算前是否对整个选区做一次终检清扫
    "final_sweep": true,

    // 边缘碎屑分片吸纳阈值（格）
    "chunk_tail_padding": 4,

    // 是否开启 SubChunk 级快速粗筛
    "subchunk_culling": true,

    // 清扫收费阶梯规格（按面积自小到大匹配第一个满足的档位）
    "price_levels": [
        { "levelname": "9*9",   "max_width": 9,  "max_depth": 9,  "price_per_layer": 1 },
        { "levelname": "17*17", "max_width": 17, "max_depth": 17, "price_per_layer": 2 },
        { "levelname": "33*33", "max_width": 33, "max_depth": 33, "price_per_layer": 4 },
        { "levelname": "65*65", "max_width": 65, "max_depth": 65, "price_per_layer": 8 }
    ]
}
```

> `price_levels` 的 `levelname` 只是**计费档位名**（如 `9*9` 表示"不超过 9×9"的档位）；
> 界面中展示的尺寸一律按选区实际坐标计算（如 `8×9`）。

---

## 7. 核心机制深度解析

### 7.1 分块清理与尾部余量吸纳 (Tail Absorption)
当末端残余宽度/深度 `rem <= chunk_tail_padding` 时自动并入前一分片。33×33 选区（分片 32×32）由 4 个分片合并为 1 个，**耗时减少 75%**。

### 7.2 液体流入拦截 (iListenAttentively)
监听 `LiquidFlowBeforeEvent`，任务处于 `Running` / `Paused` 时对流入选区的流体直接 `event.cancel()`；空间哈希网格对无关 ChunkKey 做 O(1) 短路（< 5ns）。任务完成或终止后恢复自然流动。

### 7.3 SubChunk 块级粗筛与终检清扫
利用 16×16×16 `SubChunk` 的 `isUniform(air)`，按 `mAbsoluteIndex == (pos.y >> 4)` 精确匹配，纯空气块一次跳过 4096 格；非纯空块完整点查清空（含含水方块 Layer 1）。54 万格大选区终检从 60+ 秒缩短到 1 秒以内。

### 7.4 基岩保护与绝对地板
`preserve_bottom_layers` 强制 ≥ 1；世界最低层（主世界 `y=-64`、下界/末地 `y=0`）任何配置下都不可破坏；`clearUnit` 写入前追加防挖穿硬断言。

### 7.5 并发调度与 SQLite 引擎
- 全局预算池：所有并发任务共享 `global_blocks_per_tick_budget`，时间片公平轮换；
- 语句池化：高频 SQL 通过 `Statement::cached` 常驻复用；
- 脏标记批量提交：内存为事实源，每秒 1 次单事务写入（`flushDirtyTasks`）；
- 引擎参数：256MB mmap、16MB 页缓存、内存临时表。

### 7.6 多任务看板与线框
- **槽位**：固定 `bar-slot-<i>` 复用同一条 BossBar，内容切换只发增量字段（不重建、不闪烁）；
- **轮换**：任务数 > `bossbar_max_bars` 时，只钉住剩余释放 ≤ 60 秒的暂停任务，且运行中的任务至少保留 1 个槽位，其余按 `bossbar_rotate_seconds` 滑窗轮换；
- **线框**：任务 `task-frame-<id>`（青/绿/橙/紫/粉/黄），账单 `bill-frame-<id>`（红），各自独立 key、独立颜色，互不覆盖；任务结束/账单支付或关闭即清除。

### 7.7 比例退款与离线暂存
`退款 = paidAmount × (未完成单元数 / 总单元数)`；玩家离线时写入 `pending_payouts`，下次进服自动补发并提示。

---

## 8. 数据存储与升级

数据位于 `plugins/TerritorySweeper/data/territorysweeper.db`（SQLite，WAL 模式）：

| Schema | 内容 |
| :--- | :--- |
| v2 | `tasks` / `bills` 补充领地 AABB 六列 |
| v3 | 新增 `pending_payouts`（离线退款暂存） |
| v4 | `bills` 改为 `bill_id` 自增主键 + `land_id` 索引（同一领地多张账单） |

- 迁移在启动时自动执行；结构性变更前会用 `VACUUM INTO` 生成 `territorysweeper.db.bak-<时间戳>-*.db` 一致性备份；
- 迁移在事务内完成，失败自动回滚（相关功能降级，其它数据不受影响），下次启动重试。

---

## 9. 已知取舍

1. **长加载**：不使用 BDS TickingArea，改为清理时按 `chunk_load_budget` 主动加载目标区块；
2. **基岩清理不可逆**：`clear_bedrock` 开启后会真实挖掘基岩，最底层受绝对地板保护；
3. **挂起账单无数量上限**：每张账单都需要完整选点 + 费用确认流程，不做额外限制；
4. **看板与线框按所有者展示**：只显示自己名下的任务与账单线框（他人任务/账单仅出现在领地列表中）。
