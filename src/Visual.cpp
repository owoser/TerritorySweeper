#include "Visual.h"

#include "Compat.h"
#include "Config.h"
#include "Global.h"

#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/AddActorPacket.h"
#include "mc/network/packet/BossEventPacket.h"
#include "mc/safety/RedactableString.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/ai/util/BossBarColor.h"
#include "mc/world/actor/ai/util/BossBarOverlay.h"
#include "mc/world/actor/ai/util/BossEventUpdateType.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace ts::visual {

namespace {

std::mutex& mutex() {
    static std::mutex m;
    return m;
}

float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

struct PlayerShapes {
    std::unordered_map<std::string, std::unique_ptr<compat::IBoundsBox>> boxes;
};

std::unordered_map<std::string, PlayerShapes>& shapeStore() {
    static std::unordered_map<std::string, PlayerShapes> store;
    return store;
}

std::unordered_map<std::string, int64_t>& bossIdStore() {
    static std::unordered_map<std::string, int64_t> store;
    return store;
}

std::string uuidKey(mce::UUID const& uuid) { return uuid.asString(); }

/// 维度最低高度（0 = 主世界，1 = 下界，2 = 末地）
constexpr int worldMinYFor(int dimId) { return (dimId == 1 || dimId == 2) ? 0 : -64; }

/// BossBar 的 id 同时充当 ActorUniqueID 与 ActorRuntimeID。
/// 取一个远离真实实体 id（服务器内自增的小整数）的常规正值区间。
int64_t makeBossId(std::size_t index) { return (int64_t)1'000'000'000'000LL + (int64_t)(index & 0xFFFF); }

/// 通过 BDS 工厂创建网络包。
/// 这些包头部的 mSerializationMode 由 BDS 构造函数初始化，栈上默认构造会留下脏值。
template <class TPacket>
std::shared_ptr<TPacket> createPacket(MinecraftPacketIds expected) {
    auto base = MinecraftPackets::createPacket(expected);
    if (!base) {
        log::get().warn("[bossbar] MinecraftPackets::createPacket({}) 返回空", static_cast<int>(expected));
        return nullptr;
    }
    if (base->getId() != expected) {
        log::get().warn(
            "[bossbar] 包类型不匹配：期望 {} 实际 {}",
            static_cast<int>(expected),
            static_cast<int>(base->getId())
        );
        return nullptr;
    }
    return std::static_pointer_cast<TPacket>(base);
}

/// 每玩家每 bar 的显示状态，避免重复发包与无谓的文本刷新
struct BossBarState {
    bool        shown{false};
    std::string lastTitle;
    int         lastPercent{-1};
    BarColor    lastColor{BarColor::Green};
};

std::unordered_map<std::string, BossBarState>& barStateStore() {
    static std::unordered_map<std::string, BossBarState> store;
    return store;
}

std::string barStateKey(Player const& player, int64_t bossId) {
    return player.getUuid().asString() + "/" + std::to_string(bossId);
}

BossBarState& barState(Player const& player, int64_t bossId) { return barStateStore()[barStateKey(player, bossId)]; }

/// 发送 BossEvent。只填充该事件类型需要的字段，其余保持默认。
void sendBossEvent(
    Player&                    player,
    int64_t                    bossId,
    BossEventUpdateType        type,
    std::string const*         title,
    std::optional<float>       percent01,
    std::optional<BarColor>    color
) {
    auto pkt = createPacket<BossEventPacket>(MinecraftPacketIds::BossEvent);
    if (!pkt) {
        return;
    }

    pkt->mBossID    = ActorUniqueID{bossId};
    pkt->mEventType = type;

    if (title) {
        pkt->mName = Bedrock::Safety::RedactableString{*title};
    }
    if (percent01) {
        pkt->mHealthPercent = clamp01(*percent01);
    }
    if (color) {
        pkt->mColor   = static_cast<BossBarColor>(*color);
        pkt->mOverlay = BossBarOverlay::Progress;
    }

    player.sendNetworkPacket(*pkt);

    TS_DBG(
        "[bossbar] 发包 type={} id={} percent={} player={}",
        static_cast<int>(type),
        bossId,
        percent01 ? clamp01(*percent01) : -1.0f,
        player.getRealName()
    );
}

/// 给客户端造一个看不见的玩家实体：BossBar 必须挂在实体上才能渲染
void sendFakeBossActor(Player& player, int64_t bossId) {
    auto pkt = createPacket<AddActorPacket>(MinecraftPacketIds::AddActor);
    if (!pkt) {
        return;
    }

    pkt->mEntityId  = ActorUniqueID{bossId};
    pkt->mRuntimeId = ActorRuntimeID{static_cast<uint64>(bossId)};
    pkt->mActorType = std::string{"player"};

    Vec3 pos = player.getFeetPos();
    pos.y    = static_cast<float>(worldMinYFor(static_cast<int>(player.getDimensionId())) - 64);
    pkt->mPos = pos;

    // 其余字段（速度/旋转/属性/同步数据/连接）保持默认空值，与 LSE 的做法一致
    player.sendNetworkPacket(*pkt);

    TS_DBG("[bossbar] AddActor id={} pos=({:.1f},{:.1f},{:.1f})", bossId, pos.x, pos.y, pos.z);
}

} // namespace

// ---------------------------------------------------------------------------
// BossBar
// ---------------------------------------------------------------------------

int64_t BossBar::idFor(std::string const& key) {
    std::lock_guard lock(mutex());
    auto&           store = bossIdStore();
    if (auto it = store.find(key); it != store.end()) {
        return it->second;
    }
    int64_t id = makeBossId(store.size() + 1);
    store.emplace(key, id);
    return id;
}

void BossBar::show(Player& player, int64_t bossId, std::string const& title, float progress01, BarColor color) {
    std::lock_guard lock(mutex());

    // 与 LSE PlayerClass::setBossBar 一致：先清旧条，再造实体，最后挂条形
    sendBossEvent(player, bossId, BossEventUpdateType::Remove, nullptr, std::nullopt, std::nullopt);
    sendFakeBossActor(player, bossId);
    sendBossEvent(player, bossId, BossEventUpdateType::Add, &title, progress01, color);

    auto& state   = barState(player, bossId);
    state.shown       = true;
    state.lastTitle   = title;
    state.lastPercent = static_cast<int>(std::lround(clamp01(progress01) * 100.0f));
    state.lastColor   = color;
}

void BossBar::update(Player& player, int64_t bossId, std::string const& title, float progress01, BarColor color) {
    std::lock_guard lock(mutex());

    auto& state = barState(player, bossId);
    if (!state.shown || ConfigManager::getInstance().get().bossBarUpdateMode == "recreate") {
        // 回退路径：每秒完整重建（与 LSE 行为一致，兼容性最好）
        sendBossEvent(player, bossId, BossEventUpdateType::Remove, nullptr, std::nullopt, std::nullopt);
        sendFakeBossActor(player, bossId);
        sendBossEvent(player, bossId, BossEventUpdateType::Add, &title, progress01, color);
        state.shown       = true;
        state.lastTitle   = title;
        state.lastPercent = static_cast<int>(std::lround(clamp01(progress01) * 100.0f));
        state.lastColor   = color;
        return;
    }

    // 增量路径：只发真正变化的字段，把每秒 3 个包降到 1~2 个
    if (title != state.lastTitle) {
        sendBossEvent(player, bossId, BossEventUpdateType::UpdateName, &title, std::nullopt, std::nullopt);
        state.lastTitle = title;
    }

    int const percent = static_cast<int>(std::lround(clamp01(progress01) * 100.0f));
    if (percent != state.lastPercent) {
        sendBossEvent(player, bossId, BossEventUpdateType::UpdatePercent, nullptr, progress01, std::nullopt);
        state.lastPercent = percent;
    }

    if (color != state.lastColor) {
        sendBossEvent(player, bossId, BossEventUpdateType::UpdateStyle, nullptr, std::nullopt, color);
        state.lastColor = color;
    }
}

void BossBar::hide(Player& player, int64_t bossId) {
    std::lock_guard lock(mutex());

    auto& state = barState(player, bossId);
    if (!state.shown) {
        return;
    }
    sendBossEvent(player, bossId, BossEventUpdateType::Remove, nullptr, std::nullopt, std::nullopt);
    state.shown = false;
}

void BossBar::forget(mce::UUID const& uuid) {
    std::lock_guard lock(mutex());

    auto const prefix = uuid.asString() + "/";
    auto&      states = barStateStore();
    for (auto it = states.begin(); it != states.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            it = states.erase(it);
        } else {
            ++it;
        }
    }
    bossIdStore().erase(uuidKey(uuid));
}

// ---------------------------------------------------------------------------
// RegionRenderer
// ---------------------------------------------------------------------------

void RegionRenderer::drawBox(Player& player, std::string const& key, AABB const& bounds, mce::Color const& color) {
    auto& compat = compat::DebugShapeCompat::getInstance();
    if (!compat.available()) {
        return;
    }

    auto box = compat.createBox(bounds, color);
    if (!box) {
        return;
    }

    std::lock_guard lock(mutex());
    auto&           slot = shapeStore()[uuidKey(player.getUuid())];
    if (auto it = slot.boxes.find(key); it != slot.boxes.end()) {
        it->second->remove(player);
        slot.boxes.erase(it);
    }
    box->draw(player);
    slot.boxes.emplace(key, std::move(box));
}

void RegionRenderer::clearBox(Player& player, std::string const& key) {
    std::lock_guard lock(mutex());
    auto&           store = shapeStore();
    auto            it    = store.find(uuidKey(player.getUuid()));
    if (it == store.end()) {
        return;
    }
    if (auto boxIt = it->second.boxes.find(key); boxIt != it->second.boxes.end()) {
        boxIt->second->remove(player);
        it->second.boxes.erase(boxIt);
    }
}

void RegionRenderer::clearAll(Player& player) {
    std::lock_guard lock(mutex());
    auto&           store = shapeStore();
    auto            it    = store.find(uuidKey(player.getUuid()));
    if (it == store.end()) {
        return;
    }
    for (auto& [key, box] : it->second.boxes) {
        if (box) {
            box->remove(player);
        }
    }
    store.erase(it);
}

void RegionRenderer::forget(mce::UUID const& uuid) {
    std::lock_guard lock(mutex());
    shapeStore().erase(uuidKey(uuid));
}

} // namespace ts::visual
