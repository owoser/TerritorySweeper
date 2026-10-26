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

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
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

    // 领地绑定
    int64_t     landId{0};
    std::string landName;

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
    bool    ownerInLand{true};
    bool    bossShown{false};
    int64_t paidAmount{0};

    [[nodiscard]] int  chunksPerLayer() const;
    [[nodiscard]] int  progressPercent() const;
    [[nodiscard]] bool finished() const { return layersDone >= plan.layerCount; }
};

/// 未完成工作量的千分比（0..1000），按"层 × 子分块"粒度统计
[[nodiscard]] int remainingPermille(SweepTask const& task);

/// 按未完成比例计算应退金额（以实际扣款额 paidAmount 为基准，向下取整）
[[nodiscard]] int64_t computeRefund(SweepTask const& task);

/// 挂起账单
struct PendingBill {
    std::string ownerName;
    std::string ownerXuid;
    mce::UUID   ownerUuid{};
    SweepPlan   plan;
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
    [[nodiscard]] SweepTask*  taskOfLand(int64_t landId);
    [[nodiscard]] SweepTask const* taskOfLandConst(int64_t landId) const;
    [[nodiscard]] SweepTask*  taskOfPlayer(mce::UUID const& uuid);
    [[nodiscard]] SweepTask*  taskById(uint64_t id);
    [[nodiscard]] PendingBill* billOfLand(int64_t landId);
    [[nodiscard]] PendingBill const* billOfLand(int64_t landId) const;

    /// 返回空串表示允许创建；否则为拒绝原因
    [[nodiscard]] std::string checkCreateAllowed(Player const& player, int64_t landId) const;

    // ---------------- 账单 ----------------
    void   storeBill(PendingBill bill);
    bool   dropBill(int64_t landId);
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

    void            stepTask(SweepTask& task);
    void            clearUnit(SweepTask& task);
    void            updateBossBar(SweepTask& task, Player& player);
    void            updateBossBarImpl(SweepTask& task, Player& player);
    void            loadState();
    /// 按未完成比例退款，返回实际到账金额（0 表示无需退款或退款失败）
    int64_t         refundTask(SweepTask& task, std::string const& reason);
    [[nodiscard]] Player* findOwner(SweepTask const& task) const;

    std::vector<SweepTask>   mTasks;
    std::vector<PendingBill> mBills;
    uint64_t                 mNextId{1};
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
