#include "Task.h"

#include "Config.h"
#include "Database.h"
#include "Economy.h"
#include "Global.h"
#include "Plugin.h"
#include "TaskStore.h"
#include "Visual.h"

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ts {

namespace {

constexpr int64_t kBlocksPerTickBudget = 8192;

/// 缓存的空气方块指针
Block const* airBlockPtr() {
    static Block const* ptr = []() -> Block const* {
        auto ref = Block::tryGetFromRegistry(HashedString{"minecraft:air"});
        return ref.has_value() ? &*ref : nullptr;
    }();
    return ptr;
}
constexpr char const* kBedrock = "minecraft:bedrock";

BlockSource* blockSourceFor(int dimId) {
    auto level = ll::service::getLevel();
    if (!level) {
        return nullptr;
    }
    auto ref = level->getDimension(static_cast<DimensionType>(dimId)).lock();
    if (!ref) {
        return nullptr;
    }
    return &ref->getBlockSourceFromMainChunkSource();
}

// DimensionType: 0 = 主世界, 1 = 下界, 2 = 末地
constexpr int kDimNether = 1;
constexpr int kDimTheEnd = 2;

int worldMinY(int dimId) {
    if (dimId == kDimNether || dimId == kDimTheEnd) {
        return 0;
    }
    return -64;
}

int worldMaxY(int dimId) {
    if (dimId == kDimNether) {
        return 127;
    }
    if (dimId == kDimTheEnd) {
        return 255;
    }
    return 319;
}

} // namespace

char const* toString(SweepDirection d) { return d == SweepDirection::Up ? "向上清理" : "向下清理"; }
char const* toString(TaskState s) { return s == TaskState::Running ? "运行中" : "已暂停"; }

int SweepPlan::layerY(int index) const {
    return direction == SweepDirection::Down ? (startY - index) : (startY + index);
}

int SweepTask::chunksPerLayer() const {
    if (!plan.chunked) {
        return 1;
    }
    int nx = (plan.width() + plan.chunkWidth - 1) / plan.chunkWidth;
    int nz = (plan.depth() + plan.chunkDepth - 1) / plan.chunkDepth;
    return std::max(1, nx * nz);
}

int SweepTask::progressPercent() const {
    if (plan.layerCount <= 0) {
        return 100;
    }
    int totalUnits = plan.layerCount * chunksPerLayer();
    int doneUnits  = layersDone * chunksPerLayer() + chunkIndex;
    if (totalUnits <= 0) {
        return 100;
    }
    int pct = (int)((int64_t)doneUnits * 100 / totalUnits);
    return std::clamp(pct, 0, 100);
}

// ---------------------------------------------------------------------------
// 贪心层范围计算
// ---------------------------------------------------------------------------

std::optional<LayerRange> computeLayerRange(
    Player&          player,
    SweepPlan const& bounds,
    SweepDirection   direction,
    int              manualLayers
) {
    BlockSource& source   = player.getDimensionBlockSource();
    int          dimId    = static_cast<int>(player.getDimensionId());
    int          startY   = (int)std::floor(player.getFeetPos().y);
    int          minWorld = worldMinY(dimId);
    int          maxWorld = worldMaxY(dimId);

    LayerRange range;
    range.startY = startY;

    if (manualLayers > 0) {
        range.autoComputed = false;

        if (direction == SweepDirection::Down) {
            int target = std::max(startY - manualLayers + 1, minWorld);
            range.minY  = target;
            range.maxY  = startY;
            range.count = startY - target + 1;

            // 基岩保护：向上收缩到第一层含基岩的层之上
            for (int y = startY; y >= target; --y) {
                bool bedrock = false;
                for (int x = bounds.minX; x <= bounds.maxX && !bedrock; ++x) {
                    for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
                        auto const& block = source.getBlock(BlockPos{x, y, z});
                        if (block.getTypeName() == kBedrock) {
                            bedrock = true;
                            break;
                        }
                    }
                }
                if (bedrock) {
                    range.minY  = y + 1;
                    range.count = startY - range.minY + 1;
                    range.note  = "检测到基岩层，已自动收缩到基岩之上";
                    break;
                }
            }
        } else {
            int target = std::min(startY + manualLayers - 1, maxWorld);
            range.minY  = startY;
            range.maxY  = target;
            range.count = target - startY + 1;
        }

        if (range.count <= 0) {
            return std::nullopt;
        }
        // 归一化 minY/maxY 为数值上的下/上界
        if (range.minY > range.maxY) {
            std::swap(range.minY, range.maxY);
        }
        return range;
    }

    // 自动计算
    range.autoComputed = true;

    if (direction == SweepDirection::Down) {
        int bottom = minWorld;
        bool foundBedrock = false;
        for (int y = startY; y >= minWorld; --y) {
            bool bedrock = false;
            for (int x = bounds.minX; x <= bounds.maxX && !bedrock; ++x) {
                for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
                    if (source.getBlock(BlockPos{x, y, z}).getTypeName() == kBedrock) {
                        bedrock = true;
                        break;
                    }
                }
            }
            if (bedrock) {
                bottom       = y + 1;
                foundBedrock = true;
                break;
            }
        }
        if (!foundBedrock) {
            bottom = minWorld;
        }
        range.minY  = bottom;
        range.maxY  = startY;
        range.count = startY - bottom + 1;
        range.note  = foundBedrock ? "自动扫描到基岩层之上" : "自动扫描到世界底部";
    } else {
        // 向上：找到第一个"整层全空气"的层
        int airLayer = maxWorld + 1;
        for (int y = startY; y <= maxWorld; ++y) {
            bool allAir = true;
            for (int x = bounds.minX; x <= bounds.maxX && allAir; ++x) {
                for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
                    if (!source.getBlock(BlockPos{x, y, z}).isAir()) {
                        allAir = false;
                        break;
                    }
                }
            }
            if (allAir) {
                airLayer = y;
                break;
            }
        }
        int top     = std::max(startY, airLayer - 1);
        range.minY  = startY;
        range.maxY  = top;
        range.count = top - startY + 1;
        range.note  = airLayer <= maxWorld ? "自动扫描到首个全空气层之下" : "自动扫描到世界顶部";
    }

    if (range.count <= 0) {
        return std::nullopt;
    }
    return range;
}

// ---------------------------------------------------------------------------
// TaskManager
// ---------------------------------------------------------------------------

TaskManager& TaskManager::getInstance() {
    static TaskManager instance;
    return instance;
}

Player* TaskManager::findOwner(SweepTask const& task) const {
    auto level = ll::service::getLevel();
    if (!level) {
        return nullptr;
    }
    for (auto* actor : level->getRuntimeActorList()) {
        if (!actor || !actor->isPlayer()) {
            continue;
        }
        auto& player = static_cast<Player&>(*actor);
        if (player.getUuid() == task.ownerUuid) {
            return &player;
        }
    }
    return nullptr;
}

int TaskManager::runningOrPausedCount() const { return (int)mTasks.size(); }

int TaskManager::playerTaskCount(mce::UUID const& uuid) const {
    return (int)std::count_if(mTasks.begin(), mTasks.end(), [&](SweepTask const& t) { return t.ownerUuid == uuid; });
}

SweepTask const* TaskManager::taskOfLandConst(int64_t landId) const {
    for (auto const& task : mTasks) {
        if (task.plan.landId == landId) {
            return &task;
        }
    }
    return nullptr;
}

SweepTask* TaskManager::taskOfLand(int64_t landId) {
    for (auto& task : mTasks) {
        if (task.plan.landId == landId) {
            return &task;
        }
    }
    return nullptr;
}

SweepTask* TaskManager::taskOfPlayer(mce::UUID const& uuid) {
    for (auto& task : mTasks) {
        if (task.ownerUuid == uuid) {
            return &task;
        }
    }
    return nullptr;
}

SweepTask* TaskManager::taskById(uint64_t id) {
    for (auto& task : mTasks) {
        if (task.id == id) {
            return &task;
        }
    }
    return nullptr;
}

PendingBill* TaskManager::billOfLand(int64_t landId) {
    for (auto& bill : mBills) {
        if (bill.plan.landId == landId) {
            return &bill;
        }
    }
    return nullptr;
}

PendingBill const* TaskManager::billOfLand(int64_t landId) const {
    for (auto const& bill : mBills) {
        if (bill.plan.landId == landId) {
            return &bill;
        }
    }
    return nullptr;
}

std::string TaskManager::checkCreateAllowed(Player const& player, int64_t landId) const {
    auto const& cfg = ConfigManager::getInstance().get();

    if (billOfLand(landId)) {
        return "本领地存在待支付账单，请先完成支付或关闭该账单后再新建任务。";
    }
    if (taskOfLandConst(landId)) {
        return "本领地已有进行中的清扫任务，请先完成或终止它。";
    }
    if ((int)mTasks.size() >= cfg.globalTaskLimit) {
        return "全局清理任务已满（" + std::to_string(cfg.globalTaskLimit) + "），请等待任务完成后释放名额。";
    }
    if (playerTaskCount(player.getUuid()) >= cfg.playerTaskLimit) {
        return "你已有进行中的清扫任务，请先完成或终止后再新建。";
    }
    return {};
}

void TaskManager::storeBill(PendingBill bill) {
    auto landId = bill.plan.landId;
    if (auto* existing = billOfLand(landId); existing) {
        *existing = std::move(bill);
    } else {
        mBills.push_back(std::move(bill));
    }
    if (auto* stored = billOfLand(landId); stored) {
        db::upsertBill(*stored);
    }
}

bool TaskManager::dropBill(int64_t landId) {
    auto before = mBills.size();
    std::erase_if(mBills, [&](PendingBill const& b) { return b.plan.landId == landId; });
    bool removed = mBills.size() != before;
    if (removed) {
        db::deleteBill(landId);
    }
    return removed;
}

std::string TaskManager::listBills() const {
    if (mBills.empty()) {
        return "当前没有待支付账单。";
    }
    std::string out;
    for (auto const& bill : mBills) {
        out += " - [待支付] 领地#" + std::to_string(bill.plan.landId) + " 领主 " + bill.ownerName + " 金额 "
             + std::to_string(bill.plan.totalPrice) + " 层数 " + std::to_string(bill.plan.layerCount) + "\n";
    }
    return out;
}

std::string TaskManager::listTasks() const {
    if (mTasks.empty()) {
        return "当前没有进行中的清理任务。";
    }
    std::string out;
    for (auto const& task : mTasks) {
        out += " - #" + std::to_string(task.id) + " " + task.ownerName + " | " + toString(task.state) + " | 进度 "
             + percentText(task.progressPercent()) + " (" + std::to_string(task.layersDone) + "/"
             + std::to_string(task.plan.layerCount) + " 层)\n";
    }
    return out;
}

SweepTask* TaskManager::startTask(Player& player, SweepPlan plan, bool alreadyPaid) {
    SweepTask task;
    task.id          = mNextId++;
    task.ownerName   = player.getRealName();
    task.ownerXuid   = player.getXuid();
    task.ownerUuid   = player.getUuid();
    task.plan        = std::move(plan);
    task.state       = TaskState::Running;
    task.countdown   = task.plan.layerCountdown;
    task.paidAmount  = alreadyPaid ? task.plan.totalPrice : 0;

    mTasks.push_back(std::move(task));

    auto* created = &mTasks.back();
    db::upsertTask(*created);
    db::setMetaInt("next_task_id", (int64_t)mNextId);
    log::get().info(
        "任务 #{} 已创建：领主={} 领地={} 范围={}x{} 层数={} 方向={} 分块={} 费用={}",
        created->id,
        created->ownerName,
        created->plan.landId,
        created->plan.width(),
        created->plan.depth(),
        created->plan.layerCount,
        toString(created->plan.direction),
        created->plan.chunked ? "是" : "否",
        created->plan.totalPrice
    );
    updateBossBar(*created, player);
    return created;
}

void TaskManager::pauseTask(uint64_t id, std::string const& reason) {
    auto* task = taskById(id);
    if (!task || task->state == TaskState::Paused) {
        return;
    }
    task->state         = TaskState::Paused;
    task->pausedSeconds = 0;
    task->leavePaused   = false; // 调用方若因"离开领地"触发，会在返回后置 true

    if (auto* owner = findOwner(*task); owner) {
        // 暂停期间继续显示：BossBar 变成"释放倒计时"，清理范围线框保持不消失
        task->bossShown = false;
        updateBossBar(*task, *owner);
        owner->sendMessage("§e[领地清扫] 任务已暂停：" + reason);
    }
    log::get().info("任务 #{} 已暂停：{}", id, reason);
    db::upsertTask(*task);
}

bool TaskManager::resumeTask(uint64_t id) {
    auto* task = taskById(id);
    if (!task) {
        return false;
    }
    task->state          = TaskState::Running;
    task->pausedSeconds  = 0;
    task->offLandSeconds = 0;
    task->leavePaused    = false;
    if (auto* owner = findOwner(*task); owner) {
        updateBossBar(*task, *owner);
        owner->sendMessage("§a[领地清扫] 任务已继续。");
    }
    db::upsertTask(*task);
    return true;
}

void TaskManager::finishTask(uint64_t id) {
    auto* task = taskById(id);
    if (!task) {
        return;
    }
    if (auto* owner = findOwner(*task); owner) {
        visual::BossBar::hide(*owner, visual::BossBar::idFor("task-" + std::to_string(id)));
        visual::RegionRenderer::clearBox(*owner, "task-frame");
        owner->sendMessage("§a[领地清扫] 任务已完成，感谢使用！");
    }
    log::get().info("任务 #{} 已完成。", id);
    std::erase_if(mTasks, [&](SweepTask const& t) { return t.id == id; });
    db::deleteTask(id);
}

void TaskManager::killTask(uint64_t id, std::string const& reason) {
    auto* task = taskById(id);
    if (!task) {
        return;
    }

    // 退款必须在移除任务之前计算，保证金额与实际进度一致
    int64_t const expected = computeRefund(*task);
    int64_t const refunded = refundTask(*task, reason);

    if (auto* owner = findOwner(*task); owner) {
        visual::BossBar::hide(*owner, visual::BossBar::idFor("task-" + std::to_string(id)));
        visual::RegionRenderer::clearBox(*owner, "task-frame");

        std::string message = "§c[领地清扫] 任务已终止：" + reason;
        if (refunded > 0) {
            message += "，已按未完成比例退还 " + std::to_string(refunded) + " "
                     + ConfigManager::getInstance().get().moneyname;
        } else if (expected > 0) {
            message += "，退款失败（" + std::to_string(expected) + "），请联系管理员";
        }
        owner->sendMessage(message);
    }

    log::get().info("任务 #{} 已终止：{}（退款 {}）", id, reason, refunded);
    std::erase_if(mTasks, [&](SweepTask const& t) { return t.id == id; });
    db::deleteTask(id);
}

void TaskManager::updateBossBar(SweepTask& task, Player& player) {
    auto const& cfg = ConfigManager::getInstance().get();

    // 任何表现层调用都不允许异常逃逸到 LL3 的事件分发里
    try {
        if (!cfg.bossBarEnabled && !cfg.regionFrameEnabled) {
            task.bossShown = true; // 已关闭显示，避免每轮重复进入
            return;
        }

        if (!cfg.bossBarEnabled) {
            if (!task.bossShown) {
                if (cfg.regionFrameEnabled) {
                    visual::RegionRenderer::drawBox(
                        player,
                        "task-frame",
                        AABB{
                            Vec3{(float)task.plan.minX, (float)task.plan.minY, (float)task.plan.minZ},
                            Vec3{(float)task.plan.maxX + 1.0f, (float)task.plan.maxY + 1.0f, (float)task.plan.maxZ + 1.0f}
                        },
                        mce::Color{80, 220, 255, 180}
                    );
                }
                task.bossShown = true;
            }
            return;
        }

        updateBossBarImpl(task, player);
    } catch (std::exception const& e) {
        log::get().error("刷新任务 #{} 显示时异常: {}（已停止显示该任务）", task.id, e.what());
        task.bossShown = true;
    } catch (...) {
        log::get().error("刷新任务 #{} 显示时发生未知异常（已停止显示该任务）", task.id);
        task.bossShown = true;
    }
}

void TaskManager::updateBossBarImpl(SweepTask& task, Player& player) {
    int64_t bossId = visual::BossBar::idFor("task-" + std::to_string(task.id));

    int total = task.plan.layerCount;
    int cur   = std::min(task.layersDone + 1, total);

    PhaseInfo const info  = describePhase(task);
    int const       remain = info.remainSeconds;
    int const       span   = info.phase == BarPhase::Clearing ? 0 : info.spanSeconds;

    float ratio = span > 0 ? (float)remain / (float)span : 1.0f;

    // 颜色按剩余比例，避免 2 秒的分块间隔直接闪红
    visual::BarColor color = visual::BarColor::Green;
    if (span > 0) {
        if (ratio <= 0.25f) {
            color = visual::BarColor::Red;
        } else if (ratio <= 0.5f) {
            color = visual::BarColor::Yellow;
        }
    }

    std::string title;
    if (info.phase == BarPhase::Paused) {
        title = "§c[领地清扫] §f任务已暂停 §7| §c释放倒计时 " + std::to_string(remain) + "s";
    } else {
        title = "§e[领地清扫] §f第 " + std::to_string(cur) + "/" + std::to_string(total) + " 层";

        int const chunks = task.chunksPerLayer();
        if (task.plan.chunked && chunks > 1) {
            int const chunkCur = std::min(task.chunkIndex + 1, chunks);
            title += " §7| §b分块 " + std::to_string(chunkCur) + "/" + std::to_string(chunks);
        }

        if (span > 0) {
            title += std::string{" §7| "} + info.name + " §c" + std::to_string(remain) + "s";
        } else {
            title += " §7| §a清理中";
        }
    }

    title += " §7| §a进度 " + percentText(task.progressPercent());


    if (!task.bossShown) {
        visual::BossBar::show(player, bossId, title, ratio, color);
        // 任务运行/恢复时重新显示任务区域边框（清理期间常驻）
        if (ConfigManager::getInstance().get().regionFrameEnabled) {
            visual::RegionRenderer::drawBox(
                player,
                "task-frame",
                AABB{
                    Vec3{(float)task.plan.minX, (float)task.plan.minY, (float)task.plan.minZ},
                    Vec3{(float)task.plan.maxX + 1.0f, (float)task.plan.maxY + 1.0f, (float)task.plan.maxZ + 1.0f}
                },
                mce::Color{80, 220, 255, 180}
            );
        }
        task.bossShown = true;
    } else {
        visual::BossBar::update(player, bossId, title, ratio, color);
    }
}

// ---------------------------------------------------------------------------
// 执行
// ---------------------------------------------------------------------------

void TaskManager::clearUnit(SweepTask& task) {
    BlockSource* source = blockSourceFor(task.plan.dimId);
    if (!source) {
        return;
    }

    int const totalChunks = task.chunksPerLayer();
    if (task.chunkIndex >= totalChunks) {
        return;
    }

    int const y   = task.plan.layerY(task.layersDone);
    int const cw  = task.plan.chunked ? task.plan.chunkWidth : task.plan.width();
    int const cd  = task.plan.chunked ? task.plan.chunkDepth : task.plan.depth();
    int const nx  = task.plan.chunked ? (task.plan.width() + cw - 1) / cw : 1;
    int const cz  = task.plan.chunked ? (task.plan.depth() + cd - 1) / cd : 1;

    int const ci = task.chunkIndex;
    int const cx = ci % nx;
    int const czIdx = ci / nx;

    int const x0 = task.plan.minX + cx * cw;
    int const x1 = std::min(x0 + cw - 1, task.plan.maxX);
    int const z0 = task.plan.minZ + czIdx * cd;
    int const z1 = std::min(z0 + cd - 1, task.plan.maxZ);
    (void)cz;

    Block const* airPtr = airBlockPtr();
    if (!airPtr) {
        return;
    }
    Block const& air = *airPtr;

    // 区块未加载时不能推进：否则整块会被"读成空气"而误判为已清理
    if (!isRegionLoaded(*source, BlockPos{x0, y, z0}, BlockPos{x1, y, z1})) {
        return;
    }

    task.layerHadWork = true;

    BlockChangeContext changeContext{};

    int64_t processed = 0;
    bool    finished  = false;

    for (int x = x0; x <= x1 && !finished; ++x) {
        for (int z = z0; z <= z1; ++z) {
            int64_t index = (int64_t)(x - x0) * (z1 - z0 + 1) + (z - z0);
            if (index < task.cursor) {
                continue;
            }
            BlockPos pos{x, y, z};
            auto const& block = source->getBlock(pos);
            if (!block.isAir() && block.getTypeName() != kBedrock) {
                source->setBlock(pos, air, 3, nullptr, changeContext);
            }

            ++task.cursor;
            if (++processed >= kBlocksPerTickBudget) {
                finished = false;
                break;
            }
            if (task.cursor >= (int64_t)(x1 - x0 + 1) * (z1 - z0 + 1)) {
                finished = true;
                break;
            }
        }
    }

    if (finished) {
        ++task.chunkIndex;
        task.cursor = 0;
        if (task.plan.chunked) {
            task.chunkCooldown = std::max(0, task.plan.chunkInterval);
        }
    }
}

void TaskManager::advanceEmptyUnits(SweepTask& task) {
    auto const& cfg = ConfigManager::getInstance().get();
    if (!cfg.skipEmptyChunks && !cfg.skipEmptyLayers) {
        return;
    }
    // 正在等待倒计时/间隔时不推进，避免把等待中的层又跳过去
    if (task.countdown > 0 || task.chunkCooldown > 0) {
        return;
    }

    BlockSource* source = blockSourceFor(task.plan.dimId);
    if (!source) {
        return;
    }

    int64_t budget = std::max(0, cfg.emptyScanBudget);
    if (budget <= 0) {
        return;
    }

    int const totalChunks = task.chunksPerLayer();
    int const cw  = task.plan.chunked ? task.plan.chunkWidth : task.plan.width();
    int const cd  = task.plan.chunked ? task.plan.chunkDepth : task.plan.depth();
    int const nx  = task.plan.chunked ? (task.plan.width() + cw - 1) / cw : 1;
    int64_t skippedChunks = 0;
    int     skippedLayers = 0;

    while (budget > 0) {
        if (task.chunkIndex >= totalChunks) {
            bool const previousLayerEmpty = !task.layerHadWork;

            ++task.layersDone;
            task.chunkIndex   = 0;
            task.cursor       = 0;
            task.layerHadWork = false;

            if (task.layersDone >= task.plan.layerCount) {
                return; // 最后一层已完成，交给 finished() 收尾
            }

            if (!cfg.skipEmptyLayers || !previousLayerEmpty) {
                task.countdown = task.plan.layerCountdown;
                if (skippedChunks > 0 || skippedLayers > 0) {
                    TS_DBG(
                        "[task] #{} 跳过 {} 个空分块 / {} 个空层（本层倒计时照常）",
                        task.id,
                        skippedChunks,
                        skippedLayers
                    );
                }
                return;
            }

            ++skippedLayers;
            continue; // 整层都是空的：不消耗倒计时，继续看下一层
        }

        int const ci = task.chunkIndex;
        int const x0 = task.plan.minX + (ci % nx) * cw;
        int const z0 = task.plan.minZ + (ci / nx) * cd;
        int const x1 = std::min(x0 + cw - 1, task.plan.maxX);
        int const z1 = std::min(z0 + cd - 1, task.plan.maxZ);
        int const y  = task.plan.layerY(task.layersDone);

        if (!cfg.skipEmptyChunks || !isChunkEmptyAtLayer(*source, y, x0, x1, z0, z1, budget)) {
            break; // 有活干：停下来交给 clearUnit
        }

        ++task.chunkIndex;
        task.cursor = 0;
        ++skippedChunks;
    }

    if (skippedChunks > 0 || skippedLayers > 0) {
        TS_DBG("[task] #{} 跳过 {} 个空分块 / {} 个空层", task.id, skippedChunks, skippedLayers);
    }

    // 单个 tick 跳过过多层属于异常（通常意味着区块状态异常），留一条告警便于排查
    if (skippedLayers > 32) {
        log::get().warn(
            "任务 #{} 在单次 tick 内跳过 {} 层（层 {} -> {}），请确认选区是否已全部清空。",
            task.id,
            skippedLayers,
            task.layersDone - skippedLayers + 1,
            task.layersDone
        );
    }
}

void TaskManager::stepTask(SweepTask& task) {
    if (task.finished()) {
        finishTask(task.id);
        return;
    }

    Player* owner = findOwner(task);

    // 空分块 / 空层快进（内部处理换层与倒计时设置）
    advanceEmptyUnits(task);
    if (task.finished()) {
        finishTask(task.id);
        return;
    }

    if (task.countdown > 0) {
        --task.countdown;
        if (owner) {
            updateBossBar(task, *owner);
        }
        return;
    }

    if (task.chunkCooldown > 0) {
        --task.chunkCooldown;
        if (owner) {
            updateBossBar(task, *owner);
        }
        return;
    }

    TS_DBG(
        "[task] #{} tick: layer={}/{} countdown={} chunk={}/{}",
        task.id,
        task.layersDone + 1,
        task.plan.layerCount,
        task.countdown,
        task.chunkIndex,
        task.chunksPerLayer()
    );

    if (task.chunkIndex >= task.chunksPerLayer()) {
        // advanceEmptyUnits 会处理换层；走到这里说明预算已耗尽，等下一 tick
        if (owner) {
            updateBossBar(task, *owner);
        }
        return;
    }

    clearUnit(task);

    if (owner) {
        updateBossBar(task, *owner);
    }
    db::upsertTask(task);
}

// ---------------------------------------------------------------------------
// 每秒 tick
// ---------------------------------------------------------------------------

void TaskManager::tick() {
    auto const& cfg = ConfigManager::getInstance().get();

    std::vector<uint64_t> toKill;

    for (auto& task : mTasks) {
        Player* owner = findOwner(task);

        // 领主实体缺失去抖：维度切换/实体列表抖动时不应立刻算离线
        if (!owner) {
            ++task.missingOwnerSeconds;
        } else {
            task.missingOwnerSeconds = 0;
        }
        bool const offline = !owner && task.missingOwnerSeconds >= 2;
        bool const running = (task.state == TaskState::Running);

        // 离开/离线判定只对运行中的任务生效：
        // 已暂停的任务若继续走这段，会重复调用 pauseTask 并 continue，
        // 导致 pausedSeconds 与 BossBar 一起冻结（释放倒计时卡死）。
        if (running && cfg.pauseOnPlayerLeave) {
            if (offline) {
                task.ownerInLand = false;
                ++task.offLandSeconds;
                if (task.offLandSeconds >= std::max(1, cfg.playerLeavePauseSeconds)) {
                    pauseTask(task.id, "领主已离线");
                    task.leavePaused = true;
                    continue;
                }
            } else if (owner) {
                bool const inside  = (evaluateLeaveState(task, *owner) != LeaveState::Outside);
                bool const changed = (inside != task.ownerInLand);
                task.ownerInLand   = inside;

                if (inside) {
                    task.offLandSeconds = 0;
                } else {
                    ++task.offLandSeconds;
                }

                // 仅在状态变化或每 10 秒打一条，避免刷屏
                if (changed || (task.offLandSeconds > 0 && task.offLandSeconds % 10 == 0)) {
                    auto const feet = owner->getFeetPos();
                    TS_DBG(
                        "[watchdog] task=#{} owner={} dim={} pos=({:.1f},{:.1f},{:.1f}) land=#{} inside={} off={}s",
                        task.id,
                        task.ownerName,
                        static_cast<int>(owner->getDimensionId()),
                        feet.x,
                        feet.y,
                        feet.z,
                        task.plan.landId,
                        inside,
                        task.offLandSeconds
                    );
                }

                if (!inside && task.offLandSeconds >= std::max(1, cfg.playerLeavePauseSeconds)) {
                    pauseTask(task.id, "领主离开领地超过 " + std::to_string(cfg.playerLeavePauseSeconds) + " 秒");
                    task.leavePaused = true;
                    continue;
                }
            }
        } else {
            task.offLandSeconds = 0;
            task.ownerInLand    = true;
        }

        // 只有"因离开/离线而暂停"的任务才会自动续跑；手动暂停的 leavePaused 恒为 false
        if (!running && task.leavePaused && cfg.autoResumeOnReturn && owner) {
            if (evaluateLeaveState(task, *owner) == LeaveState::Inside) {
                resumeTask(task.id);
                log::get().info("领主 {} 回到领地，任务 #{} 自动继续", task.ownerName, task.id);
                continue;
            }
        }

        if (task.state == TaskState::Paused) {
            ++task.pausedSeconds;

            if (task.pausedSeconds >= std::max(1, cfg.pauseAutoCancelSeconds)) {
                toKill.push_back(task.id);
                continue;
            }

            // 领主在线时刷新"释放倒计时"，否则无从显示
            if (owner) {
                updateBossBar(task, *owner);
            }

            // 每 5 秒落库一次，重启后可续走倒计时（崩溃最多丢 5 秒）
            if (task.pausedSeconds % 5 == 0) {
                db::upsertTask(task);
            }
            continue;
        }

        task.leavePaused = false;
        stepTask(task);
    }

    for (auto id : toKill) {
        killTask(id, "暂停超时（" + std::to_string(cfg.pauseAutoCancelSeconds) + " 秒未继续）");
    }
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------

void TaskManager::onPlayerJoin(Player& player) {
    // 进服瞬间客户端还在接收区块数据，此时直接发送 BossBar 包可能被丢弃甚至导致断连。
    // 这里只重置显示状态，真正发包交给下一轮 tick（玩家已完全进入世界）。
    if (auto* task = taskOfPlayer(player.getUuid()); task) {
        task->bossShown = false;
    }

    // 补发离线期间的退款（此时玩家在线，经济后端一定可用）
    deliverPendingPayouts(player);
}

void TaskManager::deliverPendingPayouts(Player& player) {
    auto payouts = db::loadPendingPayouts(player.getUuid().asString());
    if (payouts.empty()) {
        return;
    }

    auto&     economyService = economy::EconomyService::getInstance();
    int64_t   total          = 0;
    int       count          = 0;
    int       failed         = 0;

    for (auto const& payout : payouts) {
        economy::ChargeTarget target;
        target.xuid = payout.ownerXuid;
        target.name = payout.ownerName;
        target.uuid = player.getUuid();

        if (!economyService.add(target, payout.amount)) {
            ++failed;
            log::get().error(
                "补发离线退款失败（任务 #{}，玩家 {}，金额 {}）。",
                payout.taskId,
                payout.ownerName,
                payout.amount
            );
            continue;
        }

        db::markPayoutPaid(payout.id);
        total += payout.amount;
        ++count;
    }

    std::string const moneyname = ConfigManager::getInstance().get().moneyname;

    if (count > 0) {
        player.sendMessage(
            "§a[领地清扫] 已补发离线期间的退款 " + std::to_string(total) + " " + moneyname + "（"
            + std::to_string(count) + " 笔）"
        );
        log::get().info("已向 {} 补发离线退款 {}（{} 笔）", player.getRealName(), total, count);
    }

    if (failed > 0) {
        player.sendMessage(
            "§c[领地清扫] 有 " + std::to_string(failed) + " 笔离线退款补发失败，将在下次登录时重试。"
        );
    }
}

void TaskManager::onPlayerQuit(Player const& player) {
    // 玩家下线：隐藏 BossBar（下次上线会重建）
    if (auto* task = taskOfPlayer(player.getUuid()); task) {
        task->bossShown = false;
        visual::BossBar::forget(player.getUuid());
        visual::RegionRenderer::forget(player.getUuid());
    }
}

void TaskManager::refreshOwnerPresence() {}

// ---------------------------------------------------------------------------
// 退款计算
// ---------------------------------------------------------------------------

int remainingPermille(SweepTask const& task) {
    int const totalUnits = task.plan.layerCount * task.chunksPerLayer();
    if (totalUnits <= 0) {
        return 0;
    }
    int64_t const doneUnits = (int64_t)task.layersDone * task.chunksPerLayer() + task.chunkIndex;
    if (doneUnits <= 0) {
        return 1000;
    }
    if (doneUnits >= totalUnits) {
        return 0;
    }
    int const permille = (int)((int64_t)(totalUnits - doneUnits) * 1000 / totalUnits);
    return std::clamp(permille, 0, 1000);
}

int64_t computeRefund(SweepTask const& task) {
    if (task.paidAmount <= 0) {
        return 0;
    }
    int const totalUnits = task.plan.layerCount * task.chunksPerLayer();
    if (totalUnits <= 0) {
        return task.paidAmount;
    }
    int64_t const doneUnits = (int64_t)task.layersDone * task.chunksPerLayer() + task.chunkIndex;
    if (doneUnits <= 0) {
        return task.paidAmount;
    }
    if (doneUnits >= totalUnits) {
        return 0;
    }
    return task.paidAmount * (int64_t)(totalUnits - doneUnits) / (int64_t)totalUnits;
}

std::string percentText(int value) { return std::to_string(value) + "％"; }

PhaseInfo describePhase(SweepTask const& task) {
    PhaseInfo info;

    if (task.state == TaskState::Paused) {
        int const total    = std::max(1, ConfigManager::getInstance().get().pauseAutoCancelSeconds);
        info.phase         = BarPhase::Paused;
        info.remainSeconds = std::max(0, total - task.pausedSeconds);
        info.spanSeconds   = total;
        info.name          = "释放倒计时";
        return info;
    }

    if (task.chunkCooldown > 0) {
        info.phase         = BarPhase::ChunkInterval;
        info.remainSeconds = task.chunkCooldown;
        info.spanSeconds   = std::max(1, task.plan.chunkInterval);
        info.name          = "分块间隔";
    } else if (task.countdown > 0) {
        info.phase         = BarPhase::LayerCountdown;
        info.remainSeconds = task.countdown;
        info.spanSeconds   = std::max(1, task.plan.layerCountdown);
        info.name          = "层倒计时";
    }
    return info;
}

bool isRegionLoaded(BlockSource& source, BlockPos const& min, BlockPos const& max) {
    return source.areChunksFullyLoaded(min, max);
}

bool isChunkEmptyAtLayer(
    BlockSource& source,
    int          y,
    int          x0,
    int          x1,
    int          z0,
    int          z1,
    int64_t&     budget
) {
    // 未加载时区块内的方块一律读成空气，绝不能据此判定"无需清理"
    if (!isRegionLoaded(source, BlockPos{x0, y, z0}, BlockPos{x1, y, z1})) {
        return false;
    }

    for (int x = x0; x <= x1; ++x) {
        for (int z = z0; z <= z1; ++z) {
            if (budget <= 0) {
                return false; // 预算耗尽：保守当作非空，留到下一 tick 再判
            }
            --budget;

            auto const& block = source.getBlock(BlockPos{x, y, z});
            if (!block.isAir() && block.getTypeName() != kBedrock) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 领地内判定
// ---------------------------------------------------------------------------

int64_t landKeyOf(compat::LandSnapshot const& land) {
    if (land.id != 0) {
        return land.id;
    }

    // LandID 不可用时退化为 AABB 派生键（FNV-1a），保证不同领地不会共用同一个 0
    uint64_t hash = 1469598103934665603ull;
    auto     mix  = [&hash](int64_t value) {
        hash ^= static_cast<uint64_t>(value);
        hash *= 1099511628211ull;
    };
    mix(land.aabb.min.x);
    mix(land.aabb.min.y);
    mix(land.aabb.min.z);
    mix(land.aabb.max.x);
    mix(land.aabb.max.y);
    mix(land.aabb.max.z);

    int64_t key = static_cast<int64_t>(hash & 0x7FFFFFFFFFFFFFFFull);
    return key == 0 ? 1 : key;
}

LeaveState evaluateLeaveState(SweepTask& task, Player const& owner) {
    // 维度不同一定算离开
    if (static_cast<int>(owner.getDimensionId()) != task.plan.dimId) {
        return LeaveState::Outside;
    }

    auto const& cfg  = ConfigManager::getInstance().get();
    int const   tol  = std::max(0, cfg.leaveCheckTolerance);
    Vec3 const  feet = owner.getFeetPos();

    auto insideBox = [&](int minX, int maxX, int minY, int maxY, int minZ, int maxZ) {
        return feet.x >= (float)(minX - tol) && feet.x <= (float)(maxX + 1 + tol) && feet.z >= (float)(minZ - tol)
            && feet.z <= (float)(maxZ + 1 + tol) && feet.y >= (float)(minY - 4) && feet.y <= (float)(maxY + 8);
    };

    // 1) 任务已存领地 AABB：直接判定，不再依赖 LandID
    if (task.plan.hasLandBox()) {
        bool const inside = insideBox(
            task.plan.landMinX,
            task.plan.landMaxX,
            task.plan.landMinY,
            task.plan.landMaxY,
            task.plan.landMinZ,
            task.plan.landMaxZ
        );
        return inside ? LeaveState::Inside : LeaveState::Outside;
    }

    auto& pland = compat::PLandCompat::getInstance();
    if (!pland.available()) {
        return LeaveState::Unknown;
    }

    // 2) 按领地键反查
    std::optional<compat::LandSnapshot> snapshot = pland.landById(task.plan.landId);

    // 3) 仍拿不到：用选区中心反查一次
    if (!snapshot) {
        BlockPos const center{
            (task.plan.minX + task.plan.maxX) / 2,
            (task.plan.minY + task.plan.maxY) / 2,
            (task.plan.minZ + task.plan.maxZ) / 2
        };
        snapshot = pland.landAt(center, task.plan.dimId);
        if (snapshot) {
            log::get().info("任务 #{} 的领地范围由选区中心反查补全（landId={} 查不到）", task.id, task.plan.landId);
        }
    }

    if (!snapshot) {
        return LeaveState::Unknown;
    }

    // 回填，后续 tick 直接用 AABB，不再重复查询
    task.plan.landMinX = snapshot->aabb.min.x;
    task.plan.landMaxX = snapshot->aabb.max.x;
    task.plan.landMinY = snapshot->aabb.min.y;
    task.plan.landMaxY = snapshot->aabb.max.y;
    task.plan.landMinZ = snapshot->aabb.min.z;
    task.plan.landMaxZ = snapshot->aabb.max.z;
    if (task.plan.landName.empty()) {
        task.plan.landName = snapshot->name;
    }
    // 让领地键也收敛到当前规范值（老数据里可能存的是 0）
    if (task.plan.landId == 0) {
        task.plan.landId = landKeyOf(*snapshot);
    }

    bool const inside = insideBox(
        task.plan.landMinX,
        task.plan.landMaxX,
        task.plan.landMinY,
        task.plan.landMaxY,
        task.plan.landMinZ,
        task.plan.landMaxZ
    );
    log::get().info(
        "任务 #{} 已绑定领地范围 X {}~{} Z {}~{} Y {}~{}（inside={}）",
        task.id,
        task.plan.landMinX,
        task.plan.landMaxX,
        task.plan.landMinZ,
        task.plan.landMaxZ,
        task.plan.landMinY,
        task.plan.landMaxY,
        inside
    );
    return inside ? LeaveState::Inside : LeaveState::Outside;
}

// ---------------------------------------------------------------------------
// 持久化（SQLite）
// ---------------------------------------------------------------------------

void TaskManager::loadState() {
    mTasks  = db::loadTasks();
    mBills  = db::loadBills();
    mNextId = (uint64_t)db::getMetaInt("next_task_id", 1);
    if (mNextId == 0) {
        mNextId = 1;
    }

    if (!mBills.empty() || !mTasks.empty()) {
        log::get().info(
            "已从数据库恢复 {} 个待支付账单、{} 个清扫任务（任务统一恢复为暂停状态）",
            mBills.size(),
            mTasks.size()
        );
    }
}

/// 按未完成工作量比例退款；返回实际到账金额（失败为 0）
int64_t TaskManager::refundTask(SweepTask& task, std::string const& reason) {
    int64_t const amount = computeRefund(task);
    if (amount <= 0) {
        log::get().debug("任务 #{} 无剩余工作量，无需退款。", task.id);
        return 0;
    }

    economy::ChargeTarget target;
    target.xuid = task.ownerXuid;
    target.name = task.ownerName;
    target.uuid = task.ownerUuid;

    auto& economyService = economy::EconomyService::getInstance();

    bool const ownerOnline = findOwner(task) != nullptr;
    bool       credited    = false;

    // 经济后端（LLMoney / 计分板）都要求玩家在线；离线时改为入库暂存，等上线补发
    if (ownerOnline) {
        credited = economyService.add(target, amount);
    }

    db::recordRefund(task.id, task.ownerName, task.ownerXuid, amount, remainingPermille(task), reason);

    if (!ownerOnline) {
        PendingPayout payout;
        payout.ownerUuid = task.ownerUuid.asString();
        payout.ownerName = task.ownerName;
        payout.ownerXuid = task.ownerXuid;
        payout.amount    = amount;
        payout.reason    = reason;
        payout.taskId    = task.id;

        if (db::insertPendingPayout(payout)) {
            log::get().info(
                "任务 #{} 终止退款 {} 已暂存（玩家 {} 离线），等待其上线补发。",
                task.id,
                amount,
                task.ownerName
            );
            return amount;
        }

        log::get().error(
            "任务 #{} 的离线退款 {} 暂存失败（玩家 {}），请管理员手动补偿。",
            task.id,
            amount,
            task.ownerName
        );
        return 0;
    }

    if (!credited) {
        log::get().error(
            "退款失败（任务 #{}，玩家 {}，金额 {}，后端 {}）。任务仍会终止，请管理员手动补偿。",
            task.id,
            task.ownerName,
            amount,
            economyService.describe()
        );
        return 0;
    }

    log::get().info("任务 #{} 终止退款 {}（剩余工作量 {}‰），玩家 {}", task.id, amount, remainingPermille(task),
                    task.ownerName);
    return amount;
}

void TaskManager::init() { loadState(); }

void TaskManager::shutdown() {
    // 清理运行时表现层对象（BossBar / 线框）
    for (auto& task : mTasks) {
        if (auto* owner = findOwner(task); owner) {
            visual::BossBar::hide(*owner, visual::BossBar::idFor("task-" + std::to_string(task.id)));
            visual::RegionRenderer::clearBox(*owner, "task-frame");
        }
    }
    mTasks.clear();
    mBills.clear();
}

} // namespace ts
