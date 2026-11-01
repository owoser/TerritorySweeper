#pragma once

/**
 * 清扫任务：数据模型 + 调度器
 *
 * 任务生命周期：
 *   选点 → 方向/层数 → 计费 → 二次确认
 *        ├─ 立即支付 → Running
 *        └─ 挂起账单 → PendingPayment（按领地 ID 保存，可跨重启）
 *   Running ⇄ Paused（暂停/继续）
 *   Running/Paused → 完成或终止（kill）
 */

#include "Compat.h"

#include "mc/platform/UUID.h"
#include "mc/world/level/BlockPos.h"

class BlockSource;
class Block;

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

namespace ts {

enum class SweepDirection : uint8_t {
    Up   = 0,
    Down = 1,
};

enum class TaskState : uint8_t {
    Running = 0,
    Paused  = 1,
};

[[nodiscard]] char const* toString(SweepDirection d);
[[nodiscard]] char const* toString(TaskState s);

/// 一次清扫的完整方案（挂起账单保存的也是它）
struct SweepPlan {
    int            dimId{0};
    int            minX{0};
    int            maxX{0};
    int            minZ{0};
    int            maxZ{0};
    int            startY{0};      ///< 清理始发层（玩家脚底）
    int            minY{0};        ///< 实际清理的最低层（含）
    int            maxY{0};        ///< 实际清理的最高层（含）
    SweepDirection direction{SweepDirection::Down};
    int            layerCount{0};
    bool           layerCountAuto{false};

    // 计费（创建时固化，防止中途改配置导致价格漂移）
    std::string levelname;
    int64_t     unitPrice{0};
    int64_t     totalPrice{0};

    // 分块
    bool chunked{false};
    int  chunkWidth{10};
    int  chunkDepth{10};
    int  chunkInterval{2};
    int  layerCountdown{10};

    // 领地绑定：landId 实际存的是"领地稳定键"（优先 PLand LandID，为 0 时由 AABB 派生）
    int64_t     landId{0};
    std::string landName;
    // 领地 AABB（判定"领主是否在领地内"的依据；默认 0 表示未知）
    int         landMinX{0};
    int         landMaxX{0};
    int         landMinY{0};
    int         landMaxY{0};
    int         landMinZ{0};
    int         landMaxZ{0};

    [[nodiscard]] bool hasLandBox() const { return landMaxX > landMinX && landMaxZ > landMinZ; }

    [[nodiscard]] int  width() const { return maxX - minX + 1; }
    [[nodiscard]] int  depth() const { return maxZ - minZ + 1; }
    [[nodiscard]] int  area() const { return width() * depth(); }
    [[nodiscard]] int  layerY(int index) const; ///< index 从 0 开始
    [[nodiscard]] BlockPos minPos() const { return BlockPos{minX, minY, minZ}; }
    [[nodiscard]] BlockPos maxPos() const { return BlockPos{maxX, maxY, maxZ}; }
};

struct SweepTask {
    uint64_t   id{0};
    std::string ownerName;
    std::string ownerXuid;
    mce::UUID   ownerUuid{};
    SweepPlan   plan;
    TaskState   state{TaskState::Running};

    int     layersDone{0};       ///< 已完成的层数
    int     countdown{0};        ///< 本层剩余倒计时（秒）
    int     chunkIndex{0};       ///< 本层已完成的分块序号
    int     chunkCooldown{0};    ///< 距离下一个分块的等待秒数
    int64_t cursor{0};           ///< 当前分块内部已处理方块数
    int     pausedSeconds{0};
    int     offLandSeconds{0};
    int     missingOwnerSeconds{0}; ///< 连续找不到领主实体的秒数（去抖用）
    bool    ownerInLand{true};
    bool    leavePaused{false};     ///< 是否因"离开领地/离线"而暂停（用于自动续跑）
    bool    layerHadWork{true};     ///< 当前层是否真的清过方块（空层不消耗倒计时）
    int     waitLoadedTicks{0};     ///< 当前分块等待区块加载的秒数（仅用于告警）
    int     bedrockRemovedInLayer{0}; ///< 本层已移除的基岩数量（审计用）
    int     lowestBedrockRemovedY{0}; ///< 本层被移除基岩的最低 Y（审计用）
    int64_t finalCursor{0};           ///< 终检清扫游标
    bool    finalizing{false};        ///< 是否处于终检阶段
    bool    finalSweepDone{false};    ///< 终检是否已完成
    int64_t finalRemoved{0};          ///< 终检清除的残留方块数
    int64_t paidAmount{0};
    bool    dirty{false};             ///< 内存脏标记：仅在状态变更时置 true，由批量事务刷盘

    [[nodiscard]] int  chunksPerLayer() const;
    void getChunkGrid(int& nx, int& nz) const;
    void getChunkBounds(int ci, int& x0, int& x1, int& z0, int& z1) const;
    [[nodiscard]] int  progressPercent() const;
    [[nodiscard]] bool finished() const { return layersDone >= plan.layerCount; }
};

/// 领主是否仍在任务所属领地内
enum class LeaveState {
    Inside,  ///< 在领地内
    Outside, ///< 明确在领地外
    Unknown, ///< 无法判定（领地已删 / PLand 不可用）——按"在领地内"处理，不暂停
};

/// 领地稳定键：优先用 PLand 的 LandID；为 0 时用 AABB 派生（避免所有领地共用 0）
[[nodiscard]] int64_t landKeyOf(compat::LandSnapshot const& land);

/// 玩家可见文本里的百分比。客户端会吞掉 ASCII '%'，统一用全角 '％'
[[nodiscard]] std::string percentText(int value);

/// 选区实际尺寸文本，如 "8×9"（宽 × 深）。
/// 注意：不要用 plan.levelname（那是收费档位名，不是尺寸）。
[[nodiscard]] std::string planSizeText(SweepPlan const& plan);

/// 任务当前所处的阶段（BossBar 与管理面板共用同一套判定）
enum class BarPhase {
    Clearing,       ///< 正在清理
    LayerCountdown, ///< 层间倒计时
    ChunkInterval,  ///< 分块间隔
    Paused,         ///< 已暂停，显示释放倒计时
    Finalizing,     ///< 收尾清理（终检残留方块）
};

struct PhaseInfo {
    BarPhase    phase{BarPhase::Clearing};
    int         remainSeconds{0}; ///< 剩余秒数（Clearing 时为 0）
    int         spanSeconds{0};   ///< 该阶段总时长（Clearing 时为 0）
    char const* name{"清理中"};    ///< 阶段中文名
};

[[nodiscard]] PhaseInfo describePhase(SweepTask const& task);

/// 该方块范围是否已完全加载。未加载时 getBlock 会返回空气，不能据此判断"空"。
bool isRegionLoaded(BlockSource& source, BlockPos const& min, BlockPos const& max);

/// 检查给定坐标所在的 SubChunk 是否已经全部是空气（用于 O(1) 块级粗筛）
bool isSubChunkUniformAir(BlockSource& source, BlockPos const& pos, Block const& air);

/// 该分块在指定层是否没有任何需要清理的方块（空气与基岩都算"无需处理"）
bool isChunkEmptyAtLayer(
    BlockSource& source,
    int          dimId,
    int          y,
    int          x0,
    int          x1,
    int          z0,
    int          z1,
    int64_t&     budget
);

/// 领主当前是否仍在任务所属领地内（以领地 AABB 为准，不看清扫选区）。
/// 若任务缺少领地 AABB，会尝试反查并回填到 task.plan。
[[nodiscard]] LeaveState evaluateLeaveState(SweepTask& task, Player const& owner);

/// 未完成工作量的千分比（0..1000），按"层 × 子分块"粒度统计
[[nodiscard]] int remainingPermille(SweepTask const& task);

/// 按未完成比例计算应退金额（以实际扣款额 paidAmount 为基准，向下取整）
[[nodiscard]] int64_t computeRefund(SweepTask const& task);

/// 挂起账单（同一领地允许多张，按 id 区分）
struct PendingBill {
    int64_t     id{0}; ///< 数据库主键（0 表示尚未入库）
    std::string ownerName;
    std::string ownerXuid;
    mce::UUID   ownerUuid{};
    SweepPlan   plan;
    int64_t     createdAt{0};
};

/// 待发放的退款（玩家离线时经济后端无法入账，先入库，上线补发）
struct PendingPayout {
    int64_t     id{0};
    std::string ownerUuid;
    std::string ownerName;
    std::string ownerXuid;
    int64_t     amount{0};
    std::string reason;
    uint64_t    taskId{0};
    int64_t     createdAt{0};
};

class TaskManager {
public:
    static TaskManager& getInstance();

    void init();      ///< 读回持久化数据
    void shutdown();  ///< 持久化并清理

    /// 每秒由全局计时器驱动
    void tick();

    // ---------------- 查询 ----------------
    [[nodiscard]] int         runningOrPausedCount() const;
    [[nodiscard]] int         playerTaskCount(mce::UUID const& uuid) const;
    /// 该领地的第一个任务（仅用于"关闭同一领地多任务"时的冲突判定）
    [[nodiscard]] SweepTask const* taskOfLandConst(int64_t landId) const;
    /// 该领地内的全部任务（按 id 升序）
    [[nodiscard]] std::vector<SweepTask*> tasksOfLand(int64_t landId);
    /// 该玩家的全部任务（按 id 升序）
    [[nodiscard]] std::vector<SweepTask*> tasksOfPlayer(mce::UUID const& uuid);
    [[nodiscard]] SweepTask*  taskById(uint64_t id);
    /// 该领地内的全部待支付账单（按 id 升序）
    [[nodiscard]] std::vector<PendingBill*> billsOfLand(int64_t landId);
    [[nodiscard]] std::vector<PendingBill const*> billsOfLand(int64_t landId) const;
    /// 该玩家名下的全部待支付账单（线框展示用）
    [[nodiscard]] std::vector<PendingBill const*> billsOfPlayer(mce::UUID const& uuid) const;
    [[nodiscard]] PendingBill* billById(int64_t billId);

    /// 该坐标是否落在某个任务（运行中或已暂停）的清理范围内（液体拦截用）
    [[nodiscard]] bool isInsideActiveTask(int dimId, BlockPos const& pos) const;

    /// 返回空串表示允许创建；否则为拒绝原因
    /// 说明：挂起账单不占任务名额，也不阻止新建任务（多账单下才有意义），支付时才校验名额
    [[nodiscard]] std::string checkCreateAllowed(Player const& player, int64_t landId) const;

    // ---------------- 账单 ----------------
    /// 追加一张挂起账单（分配 id 并入库）；同一领地允许多张
    void   storeBill(PendingBill bill);
    bool   dropBill(int64_t billId);
    [[nodiscard]] std::string listBills() const;

    // ---------------- 任务 ----------------
    SweepTask* startTask(Player& player, SweepPlan plan, bool alreadyPaid);
    void       pauseTask(uint64_t id, std::string const& reason);
    bool       resumeTask(uint64_t id);
    void       killTask(uint64_t id, std::string const& reason);
    void       finishTask(uint64_t id);

    // ---------------- 事件 ----------------
    void onPlayerJoin(Player& player);
    void onPlayerQuit(Player const& player);
    void refreshOwnerPresence();

    [[nodiscard]] std::string listTasks() const;

private:
    TaskManager() = default;

    void            stepTask(SweepTask& task, int64_t& globalBudget);
    void            advanceEmptyUnits(SweepTask& task);
    /// 任务结束前的终检清扫：清除选区体积内残留的液体/方块
    void            runFinalSweep(SweepTask& task, int64_t& globalBudget);
    /// 按每秒预算主动加载该水平范围覆盖的区块
    void            loadChunksForRange(BlockSource& source, int x0, int x1, int z0, int z1);
    /// 把离线期间暂存的退款发放给该玩家
    void            deliverPendingPayouts(Player& player);
    void            clearUnit(SweepTask& task, int64_t& budget);
    /// 重建活跃任务所覆盖的空间区块网格索引（加速液体流入 O(1) 短路判定）
    void            rebuildActiveChunkGrid();
    /// 兼容旧调用点：刷新该玩家的 BossBar 看板（含轮换与线框）
    void            updateBossBar(SweepTask& task, Player& player);
    /// 该玩家任务的展示顺序：暂停优先（释放剩余时间升序）→ 运行中（id 升序）
    [[nodiscard]] std::vector<SweepTask*> orderedTasksOfPlayer(mce::UUID const& uuid);
    /// 刷新全部在线领主的看板（tick 末尾兜底，保证轮换持续）
    void            refreshBoards();
    /// 刷新单个玩家的槽位分配与线框
    void            refreshBoard(Player& player);
    /// 渲染一个槽位（首次 show，之后一律增量 update，避免轮换闪烁）
    void            renderSlot(Player& player, int slotIndex, SweepTask& task, bool multi, bool rotating);
    /// 为每个任务/账单各画一条独立线框（任务按 id 配色，账单固定红色），消失时自动清除
    void            refreshFrames(
        Player&                                player,
        std::vector<SweepTask*> const&         tasks,
        std::vector<PendingBill const*> const& bills
    );
    void            loadState();
    /// 按未完成比例退款，返回实际到账金额（0 表示无需退款或退款失败）
    int64_t         refundTask(SweepTask& task, std::string const& reason);
    [[nodiscard]] Player* findOwner(SweepTask const& task) const;
    [[nodiscard]] Player* findPlayerByUuid(mce::UUID const& uuid) const;

    /// BossBar 单个槽位：固定 bossId 复用，内容轮换时只发增量更新
    struct BarSlot {
        uint64_t taskId{0}; ///< 0 表示空槽
        bool     shown{false};
    };

    /// 单个玩家的 BossBar 看板
    struct BarBoard {
        std::vector<BarSlot>         slots;        ///< 长度 = bossbar_max_bars
        std::unordered_set<uint64_t> framedTasks; ///< 已绘制线框的任务 id（每任务一条框）
        std::unordered_set<int64_t>  framedBills; ///< 已绘制线框的账单 id（每账单一条框）
        int                          failures{0}; ///< 连续异常次数，>=3 停用该玩家看板
    };

    std::unordered_map<std::string, BarBoard> mBoards; ///< key = 玩家 UUID 字符串

    int                      mChunkLoadBudget{0}; ///< 本 tick 剩余的区块加载额度
    std::unordered_set<uint64_t> mActiveChunkKeys; ///< 空间哈希网格
    std::vector<SweepTask>   mTasks;
    std::vector<PendingBill> mBills;
    uint64_t                 mNextId{1};
    int64_t                  mNextBillId{1}; ///< 挂起账单的自增 id
    int64_t                  mLastTick{0};
};

// ---------------------------------------------------------------------------
// 层范围计算（供表单/指令复用）
// ---------------------------------------------------------------------------

struct LayerRange {
    int  startY{0};
    int  minY{0};
    int  maxY{0};
    int  count{0};
    bool autoComputed{false};
    std::string note;
};

/// 计算清理层范围。manualLayers<=0 表示自动计算。
[[nodiscard]] std::optional<LayerRange> computeLayerRange(
    Player&             player,
    SweepPlan const&    bounds,
    SweepDirection      direction,
    int                 manualLayers
);

} // namespace ts
