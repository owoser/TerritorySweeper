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

#include <memory>
#include <string>
#include <vector>

namespace ts::interaction {

namespace {

constexpr char const* kStickItemName = "minecraft:stick";

std::vector<std::shared_ptr<ll::event::ListenerBase>> gListeners;
int                                                   gTickCounter = 0;

std::string coordinateText(BlockPos const& pos) {
    return "(" + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z) + ")";
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

/// 木棍点击的统一定点逻辑
void applyPoint(Player& player, PlayerLand const& land, BlockPos const& pos) {
    auto& selectionManager = SelectionManager::getInstance();

    auto state = selectionManager.get(player);
    if (!state) {
        auto reject = TaskManager::getInstance().checkCreateAllowed(player, land.land.id);
        if (!reject.empty()) {
            player.sendMessage("§c" + reject);
            return;
        }
        selectionManager.begin(player, land);
        state = selectionManager.get(player);
        player.sendMessage("§e[领地清扫] 已开启领地边框显示，请使用木棍点击第 1 个方块设置点 A。");
        if (!state) {
            return;
        }
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

    // 已完成选点，再点方块直接打开方向菜单
    if (auto fresh = selectionManager.get(player); fresh && fresh->complete()) {
        form::openDirection(player, land, *fresh);
    }
}

void handleInteract(Player& player, BlockPos const& blockPos) {
    if (!holdingStick(player)) {
        return;
    }

    auto land = resolveLandUnderPlayer(player);
    if (!land) {
        player.sendMessage("§c[领地清扫] 你不在任何领地内。");
        return;
    }
    if (!land->canManage()) {
        player.sendMessage("§c[领地清扫] 只有领地主人或管理员可以使用该功能。");
        return;
    }

    auto& manager = TaskManager::getInstance();

    if (auto* task = manager.taskOfLand(land->land.id); task) {
        form::openManage(player, *task);
        return;
    }
    if (auto* bill = manager.billOfLand(land->land.id); bill) {
        form::openBill(player, *bill);
        return;
    }

    auto state = SelectionManager::getInstance().get(player);
    if (state && state->complete()) {
        form::openDirection(player, *land, *state);
        return;
    }

    applyPoint(player, *land, blockPos);
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
            // 木棍点击地面不应触发任何物品行为
            handleInteract(player, event.blockPos());
        }
    ));

    gListeners.emplace_back(bus.emplaceListener<ll::event::player::PlayerDestroyBlockEvent>(
        [](ll::event::player::PlayerDestroyBlockEvent& event) {
            Player& player = event.self();
            if (!holdingStick(player)) {
                return;
            }
            // 左键点方块：用于设置点 A，同时阻止木棍真的去破坏方块
            event.cancel();
            auto land = resolveLandUnderPlayer(player);
            if (!land) {
                player.sendMessage("§c[领地清扫] 你不在任何领地内。");
                return;
            }
            if (!land->canManage()) {
                player.sendMessage("§c[领地清扫] 只有领地主人或管理员可以使用该功能。");
                return;
            }
            if (auto* task = TaskManager::getInstance().taskOfLand(land->land.id); task) {
                form::openManage(player, *task);
                return;
            }
            if (auto* bill = TaskManager::getInstance().billOfLand(land->land.id); bill) {
                form::openBill(player, *bill);
                return;
            }
            applyPoint(player, *land, event.pos());
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
