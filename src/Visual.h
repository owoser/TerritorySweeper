#pragma once

/**
 * 视觉反馈层：
 *   - BossBar：基于 BDS 原生 BossEventPacket 的进度条（零实体、零存档）
 *   - RegionRenderer：基于 DebugShape 的玩家私有线框（领地边框 / 清扫区域边框）
 */

#include "mc/deps/core/math/Color.h"
#include "mc/platform/UUID.h"
#include "mc/world/phys/AABB.h"

#include <cstdint>
#include <string>

class Player;

namespace ts::visual {

// ---------------------------------------------------------------------------
// BossBar
// ---------------------------------------------------------------------------

/// BossBar 颜色（与 BDS BossBarColor 枚举值一致）
enum class BarColor : unsigned char {
    Pink          = 0,
    Blue          = 1,
    Red           = 2,
    Green         = 3,
    Yellow        = 4,
    Purple        = 5,
    RebeccaPurple = 6,
    White         = 7,
};

class BossBar {
public:
    /// 为指定键生成一个稳定的 bossId（同一键每次返回相同值）
    [[nodiscard]] static int64_t idFor(std::string const& key);

    static void show(Player& player, int64_t bossId, std::string const& title, float progress01, BarColor color);
    static void update(Player& player, int64_t bossId, std::string const& title, float progress01, BarColor color);
    static void hide(Player& player, int64_t bossId);

    /// 玩家断线时清空其 bossId 映射
    static void forget(mce::UUID const& uuid);
};

// ---------------------------------------------------------------------------
// RegionRenderer
// ---------------------------------------------------------------------------

class RegionRenderer {
public:
    /// 仅对该玩家绘制一个盒状线框（同一 key 会先移除旧框线）
    static void drawBox(
        Player&             player,
        std::string const&  key,
        AABB const&         bounds,
        mce::Color const&   color
    );

    /// 移除该玩家的指定线框
    static void clearBox(Player& player, std::string const& key);

    /// 移除该玩家的全部线框
    static void clearAll(Player& player);

    /// 玩家断线时清理（避免残留对象）
    static void forget(mce::UUID const& uuid);
};

} // namespace ts::visual
