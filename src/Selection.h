#pragma once

/**
 * 选区与领地上下文
 *
 * 领地校验的关键简化：只校验"玩家当前脚下坐标"所在领地，不要求整个选区落在同一领地。
 * 选区点本身再逐点校验是否落在该领地 AABB 内。
 */

#include "Compat.h"

#include "mc/platform/UUID.h"
#include "mc/world/level/BlockPos.h"

#include <optional>
#include <string>
#include <unordered_map>

class Player;

namespace ts {

/// 玩家脚下所在的领地 + 权限判定
struct PlayerLand {
    compat::LandSnapshot land;
    compat::LandRole     role{compat::LandRole::Actor};
    bool                 isOperator{false};

    [[nodiscard]] bool canManage() const {
        return isOperator || role == compat::LandRole::Owner || role == compat::LandRole::Admin;
    }
    [[nodiscard]] std::string roleName() const;
};

/// 玩家脚底所在的方块坐标
[[nodiscard]] BlockPos feetBlock(Player const& player);

/// 解析玩家脚下坐标所在领地（必要时做 PLand 权限判定）
[[nodiscard]] std::optional<PlayerLand> resolveLandUnderPlayer(Player const& player);

struct SelectionState {
    bool     active{false};
    bool     hasPos1{false};
    bool     hasPos2{false};
    BlockPos pos1{};
    BlockPos pos2{};
    int      dimId{0};
    int64_t  landId{0};

    [[nodiscard]] bool complete() const { return hasPos1 && hasPos2; }
};

class SelectionManager {
public:
    static SelectionManager& getInstance();

    void begin(Player& player, PlayerLand const& land);
    void cancel(Player& player);
    void clear(Player& player);

    [[nodiscard]] bool                         isActive(Player const& player) const;
    [[nodiscard]] std::optional<SelectionState> get(Player const& player) const;

    /// which: 1 = A 点, 2 = B 点
    bool setPoint(Player& player, int which, BlockPos const& pos);

    void onPlayerQuit(Player const& player);

    /// 选区外框（DebugShape）
    void refreshBox(Player& player, SelectionState const& state);

private:
    SelectionManager() = default;

    std::unordered_map<std::string, SelectionState> mStates;
};

} // namespace ts
