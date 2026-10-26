#include "Interaction.h"

#include "Config.h"
#include "Forms.h"
#include "Global.h"
#include "Selection.h"
#include "Task.h"
#include "Visual.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "mc/world/actor/player/Player.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts::interaction {

namespace {

constexpr char const* kStickItemName = "minecraft:stick";

std::vector<std::shared_ptr<ll::event::ListenerBase>> gListeners;
int                                                   gTickCounter = 0;

std::string coordinateText(BlockPos const& pos) {
    return "(" + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z) + ")";
}

/// 同一玩家 150ms 内的重复木棍点击只处理一次（BDS 对同一次点击可能触发多个事件）
bool isDuplicateClick(Player const& player) {
    using Clock = std::chrono::steady_clock;

    static std::mutex                                     mutex;
    static std::unordered_map<std::string, Clock::time_point> lastClick;

    auto const key = player.getUuid().asString();
    auto const now = Clock::now();

    std::lock_guard lock(mutex);
    if (auto it = lastClick.find(key); it != lastClick.end() && now - it->second < std::chrono::milliseconds(150)) {
        return true;
    }
    lastClick[key] = now;
    return false;
}

bool holdingStick(Player& player) {
    auto& item = player.getCarriedItem();
    return item.getTypeName() == kStickItemName;
}

void rejectOutOfLand(Player& player, PlayerLand const& land, BlockPos const& pos) {
    player.sendMessage(
        "§c[领地清扫] 选区点超出领地范围！领地范围 X " + std::to_string(land.land.aabb.min.x) + "~"
        + std::to_string(land.land.aabb.max.x) + "  Y " + std::to_string(land.land.aabb.min.y) + "~"
        + std::to_string(land.land.aabb.max.y) + "  Z " + std::to_string(land.land.aabb.min.z) + "~"
        + std::to_string(land.land.aabb.max.z)
    );
}

/// 木棍点击的定点逻辑（仅在有活动选区时调用）
void applyPoint(Player& player, PlayerLand const& land, BlockPos const& pos) {
    auto& selectionManager = SelectionManager::getInstance();

    auto state = selectionManager.get(player);
    if (!state || !state->active) {
        return;
    }

    if (!land.land.contains(pos)) {
        rejectOutOfLand(player, land, pos);
        return;
    }

    if (!state->hasPos1) {
        selectionManager.setPoint(player, 1, pos);
        selectionManager.refreshBox(player, *selectionManager.get(player));
        player.sendMessage("§a[领地清扫] 点 A 已设置 " + coordinateText(pos));
        player.sendMessage("§e请使用木棍点击第 2 个方块设置点 B。");
        return;
    }

    if (!state->hasPos2) {
        selectionManager.setPoint(player, 2, pos);
        auto finalState = selectionManager.get(player);
        if (finalState) {
            selectionManager.refreshBox(player, *finalState);
        }
        player.sendMessage("§a[领地清扫] 点 B 已设置 " + coordinateText(pos));
        // 选点完成：关闭领地边框，仅保留选区框
        visual::RegionRenderer::clearBox(player, "land-frame");
        player.sendMessage("§a[领地清扫] 选点完成，请使用木棍点击地面打开任务菜单。");
        return;
    }

}

/// 一次木棍点击所对应的上下文；enabled 为 false 时调用方应完全静默
struct StickContext {
    std::optional<PlayerLand>     land;
    SweepTask*                    task{nullptr};
    PendingBill*                  bill{nullptr};
    std::optional<SelectionState> selection;
    bool                          enabled{false};
};

/// 统一判定"这一次木棍点击是否归我们管"：
///   1) 必须手持木棍（调用方已判断）
///   2) 配置要求下蹲时必须在下蹲
///   3) 必须处于三种状态之一：已 /ts new 选点、领地内有任务、领地内有待支付账单
StickContext resolveStickContext(Player& player) {
    StickContext context;

    auto const& cfg = ConfigManager::getInstance().get();
    if (cfg.stickRequiresSneak && !player.isSneaking()) {
        return context;
    }

    context.selection = SelectionManager::getInstance().get(player);

    if (auto land = resolveLandUnderPlayer(player); land) {
        auto& manager = TaskManager::getInstance();
        context.task  = manager.taskOfLand(land->key);
        context.bill  = manager.billOfLand(land->key);
        context.land  = std::move(land);
    }

    bool const hasSelection = context.selection && context.selection->active;
    context.enabled         = hasSelection || context.task != nullptr || context.bill != nullptr;
    return context;
}

/// 木棍交互统一入口（左键 / 右键 / 手机点击 共用）
void handleStickUse(Player& player, BlockPos const& pos) {
    auto context = resolveStickContext(player);
    if (!context.enabled) {
        // 未 /ts new、领地内也没有任务/账单时，完全静默：不提示、不处理
        return;
    }

    // 同一次点击有时会触发多个事件，150ms 内只处理第一次
    if (isDuplicateClick(player)) {
        return;
    }

    if (!context.land) {
        player.sendMessage("§c[领地清扫] 你不在任何领地内。");
        return;
    }
    if (!context.land->canManage()) {
        player.sendMessage("§c[领地清扫] 只有领地主人或管理员可以使用该功能。");
        return;
    }

    TS_DBG(
        "[interact] 木棍点击 {} 领地#{} 角色={} 坐标=({},{},{})",
        player.getRealName(),
        context.land->key,
        context.land->roleName(),
        pos.x,
        pos.y,
        pos.z
    );

    if (context.task) {
        TS_DBG("[interact] 命中任务 #{} → 打开管理面板", context.task->id);
        form::openManage(player, *context.task);
        return;
    }
    if (context.bill) {
        form::openBill(player, *context.bill);
        return;
    }

    if (!context.selection) {
        return;
    }

    if (context.selection->complete()) {
        form::openDirection(player, *context.land, *context.selection);
        return;
    }

    applyPoint(player, *context.land, pos);
}

} // namespace

void registerListeners() {
    auto& bus = ll::event::EventBus::getInstance();

    gListeners.emplace_back(bus.emplaceListener<ll::event::player::PlayerInteractBlockEvent>(
        [](ll::event::player::PlayerInteractBlockEvent& event) {
            Player& player = event.self();
            if (!holdingStick(player)) {
                return;
            }
            handleStickUse(player, event.blockPos());
        }
    ));

    gListeners.emplace_back(bus.emplaceListener<ll::event::player::PlayerDestroyBlockEvent>(
        [](ll::event::player::PlayerDestroyBlockEvent& event) {
            Player& player = event.self();
            if (!holdingStick(player)) {
                return;
            }
            // 只有确实归我们管（选点中 / 有任务 / 有账单）才拦截破坏，否则木棍正常挖方块
            if (!resolveStickContext(player).enabled) {
                return;
            }
            // 左键点方块：用于设置点 A，同时阻止木棍真的去破坏方块
            event.cancel();
            handleStickUse(player, event.pos());
        }
    ));

    gListeners.emplace_back(bus.emplaceListener<ll::event::player::PlayerJoinEvent>(
        [](ll::event::player::PlayerJoinEvent& event) { TaskManager::getInstance().onPlayerJoin(event.self()); }
    ));

    gListeners.emplace_back(bus.emplaceListener<ll::event::player::PlayerDisconnectEvent>(
        [](ll::event::player::PlayerDisconnectEvent& event) {
            auto& player = event.self();
            TaskManager::getInstance().onPlayerQuit(player);
            SelectionManager::getInstance().onPlayerQuit(player);
        }
    ));

    gListeners.emplace_back(bus.emplaceListener<ll::event::world::ServerLevelTickEvent>(
        [](ll::event::world::ServerLevelTickEvent&) {
            if (++gTickCounter < 20) {
                return;
            }
            gTickCounter = 0;
            try {
                TaskManager::getInstance().tick();
            } catch (std::exception const& e) {
                log::get().error("清扫任务 tick 异常: {}", e.what());
            } catch (...) {
                log::get().error("清扫任务 tick 发生未知异常");
            }
        }
    ));

    log::get().info("已注册 {} 个事件监听器。", gListeners.size());
}

void unregisterListeners() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : gListeners) {
        if (listener) {
            bus.removeListener(listener);
        }
    }
    gListeners.clear();
}

} // namespace ts::interaction
