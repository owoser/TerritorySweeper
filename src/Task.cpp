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
             + std::to_string(task.progressPercent()) + "% (" + std::to_string(task.layersDone) + "/"
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
    task->state        = TaskState::Paused;
    task->pausedSeconds = 0;

    if (auto* owner = findOwner(*task); owner) {
        visual::BossBar::hide(*owner, visual::BossBar::idFor("task-" + std::to_string(task->id)));
        visual::RegionRenderer::clearBox(*owner, "task-frame");
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
    task->state         = TaskState::Running;
    task->pausedSeconds = 0;
    task->offLandSeconds = 0;
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

    int total  = task.plan.layerCount;
    int cur    = std::min(task.layersDone + 1, total);
    int remain = std::max(task.countdown, 0);

    float ratio = task.plan.layerCountdown > 0
                    ? (float)remain / (float)task.plan.layerCountdown
                    : 1.0f;

    visual::BarColor color = visual::BarColor::Green;
    if (remain <= 3) {
        color = visual::BarColor::Red;
    } else if (remain <= 6) {
        color = visual::BarColor::Yellow;
    }

    std::string title = "§e[领地清扫] 第 " + std::to_string(cur) + "/" + std::to_string(total) + " 层 | 倒计时 §c"
                      + std::to_string(remain) + "s §7| §a进度 " + std::to_string(task.progressPercent()) + "%";

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

    static Block const* airPtr = []() -> Block const* {
        auto ref = Block::tryGetFromRegistry(HashedString{"minecraft:air"});
        return ref.has_value() ? &*ref : nullptr;
    }();
    if (!airPtr) {
        return;
    }
    Block const& air = *airPtr;

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

void TaskManager::stepTask(SweepTask& task) {
    if (task.finished()) {
        finishTask(task.id);
        return;
    }

    Player* owner = findOwner(task);

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

    int const totalChunks = task.chunksPerLayer();
    if (task.chunkIndex >= totalChunks) {
        ++task.layersDone;
        task.chunkIndex = 0;
        task.cursor     = 0;
        if (task.layersDone >= task.plan.layerCount) {
            finishTask(task.id);
            return;
        }
        task.countdown = task.plan.layerCountdown;
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

        // 玩家在线状态与是否在领地内
        if (!owner) {
            task.ownerInLand = false;
            if (cfg.pauseOnPlayerLeave) {
                ++task.offLandSeconds;
                if (task.offLandSeconds >= std::max(1, cfg.playerLeavePauseSeconds)) {
                    pauseTask(task.id, "领主已离线");
                    continue;
                }
            }
        } else {
            bool inside = static_cast<int>(owner->getDimensionId()) == task.plan.dimId
                       && owner->getFeetPos().x >= task.plan.minX - 0.5f
                       && owner->getFeetPos().x <= task.plan.maxX + 1.5f
                       && owner->getFeetPos().z >= task.plan.minZ - 0.5f
                       && owner->getFeetPos().z <= task.plan.maxZ + 1.5f
                       && owner->getFeetPos().y >= task.plan.minY - 4.0f
                       && owner->getFeetPos().y <= task.plan.maxY + 8.0f;

            task.ownerInLand = inside;
            if (inside) {
                task.offLandSeconds = 0;
            } else if (cfg.pauseOnPlayerLeave) {
                ++task.offLandSeconds;
                if (task.offLandSeconds >= std::max(1, cfg.playerLeavePauseSeconds)) {
                    pauseTask(task.id, "领主离开领地超过 " + std::to_string(cfg.playerLeavePauseSeconds) + " 秒");
                    continue;
                }
            }
        }

        if (task.state == TaskState::Paused) {
            ++task.pausedSeconds;
            if (task.pausedSeconds >= std::max(1, cfg.pauseAutoCancelSeconds)) {
                toKill.push_back(task.id);
                continue;
            }
            continue;
        }

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
        (void)player;
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
    bool  credited       = economyService.add(target, amount);

    db::recordRefund(task.id, task.ownerName, task.ownerXuid, amount, remainingPermille(task), reason);

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
