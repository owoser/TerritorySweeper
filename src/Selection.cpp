#include "Selection.h"

#include "Config.h"
#include "Global.h"
#include "Task.h"
#include "Visual.h"

#include "mc/world/actor/player/Player.h"

#include <cmath>
#include <mutex>

namespace ts {

namespace {

std::mutex& mutex() {
    static std::mutex m;
    return m;
}

std::string keyOf(Player const& player) { return player.getUuid().asString(); }

AABB landBox(compat::LandAABB const& aabb) {
    Vec3 min{(float)aabb.min.x, (float)aabb.min.y, (float)aabb.min.z};
    Vec3 max{(float)aabb.max.x + 1.0f, (float)aabb.max.y + 1.0f, (float)aabb.max.z + 1.0f};
    return AABB{min, max};
}

} // namespace

std::string PlayerLand::roleName() const {
    if (isOperator) {
        return "管理员";
    }
    switch (role) {
    case compat::LandRole::Admin:
        return "领地管理";
    case compat::LandRole::Owner:
        return "领地主人";
    case compat::LandRole::Member:
        return "领地成员";
    case compat::LandRole::Actor:
        return "访客";
    default:
        return "未知";
    }
}

BlockPos feetBlock(Player const& player) {
    auto pos = player.getFeetPos();
    return BlockPos{(int)std::floor(pos.x), (int)std::floor(pos.y), (int)std::floor(pos.z)};
}

std::optional<PlayerLand> resolveLandUnderPlayer(Player const& player) {
    auto& pland = compat::PLandCompat::getInstance();
    if (!pland.available()) {
        return std::nullopt;
    }

    int      dimId = static_cast<int>(player.getDimensionId());
    BlockPos feet  = feetBlock(player);

    auto land = pland.landAt(feet, dimId);
    if (!land) {
        return std::nullopt;
    }

    PlayerLand result;
    result.land       = std::move(*land);
    result.isOperator = pland.isOperator(player.getUuid());
    result.key        = landKeyOf(result.land);
    if (auto role = pland.roleOf(result.land, player.getUuid()); role) {
        result.role = *role;
    }
    return result;
}

SelectionManager& SelectionManager::getInstance() {
    static SelectionManager instance;
    return instance;
}

void SelectionManager::begin(Player& player, PlayerLand const& land) {
    std::lock_guard lock(mutex());

    SelectionState state;
    state.active = true;
    state.dimId  = land.land.dimId;
    state.landId = land.key;
    mStates[keyOf(player)] = state;

    TS_DBG("[selection] begin 玩家={} 领地#{}", player.getRealName(), land.key);

    if (ConfigManager::getInstance().get().regionFrameEnabled) {
        visual::RegionRenderer::drawBox(
            player,
            "land-frame",
            landBox(land.land.aabb),
            mce::Color{255, 215, 0, 200} // 金色：领地边界
        );
    }
}

void SelectionManager::cancel(Player& player) {
    std::lock_guard lock(mutex());
    mStates.erase(keyOf(player));
    visual::RegionRenderer::clearBox(player, "land-frame");
    visual::RegionRenderer::clearBox(player, "selection-frame");
}

void SelectionManager::clear(Player& player) {
    std::lock_guard lock(mutex());
    mStates.erase(keyOf(player));
    visual::RegionRenderer::clearBox(player, "land-frame");
    visual::RegionRenderer::clearBox(player, "selection-frame");
}

bool SelectionManager::isActive(Player const& player) const {
    std::lock_guard lock(mutex());
    auto            it = mStates.find(keyOf(player));
    return it != mStates.end() && it->second.active && !it->second.complete();
}

std::optional<SelectionState> SelectionManager::get(Player const& player) const {
    std::lock_guard lock(mutex());
    auto            it = mStates.find(keyOf(player));
    if (it == mStates.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool SelectionManager::setPoint(Player& player, int which, BlockPos const& pos) {
    std::lock_guard lock(mutex());

    auto key = keyOf(player);
    auto it  = mStates.find(key);
    if (it == mStates.end()) {
        return false;
    }

    auto& state = it->second;

    state.active = true;
    if (which == 1) {
        state.pos1   = pos;
        state.hasPos1 = true;
    } else {
        state.pos2   = pos;
        state.hasPos2 = true;
    }
    return true;
}

void SelectionManager::refreshBox(Player& player, SelectionState const& state) {
    if (!state.hasPos1 && !state.hasPos2) {
        return;
    }
    if (!ConfigManager::getInstance().get().regionFrameEnabled) {
        return;
    }

    BlockPos a = state.hasPos1 ? state.pos1 : state.pos2;
    BlockPos b = state.hasPos2 ? state.pos2 : state.pos1;

    int x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
    int y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    int z0 = std::min(a.z, b.z), z1 = std::max(a.z, b.z);

    // 只画水平范围，高度显示 1 层，避免遮挡视线
    visual::RegionRenderer::drawBox(
        player,
        "selection-frame",
        AABB{Vec3{(float)x0, (float)y0, (float)z0}, Vec3{(float)x1 + 1.0f, (float)(y1 + 1), (float)z1 + 1.0f}},
        mce::Color{80, 220, 255, 200}
    );
}

void SelectionManager::onPlayerQuit(Player const& player) {
    std::lock_guard lock(mutex());
    mStates.erase(keyOf(player));
}

} // namespace ts
