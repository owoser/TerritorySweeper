#include "Visual.h"

#include "Compat.h"
#include "Global.h"

#include "mc/legacy/ActorUniqueID.h"
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

BossEventPacket makeBossPacket(
    int64_t             bossId,
    BossEventUpdateType type,
    std::string const&  title,
    float               progress01,
    BarColor            color
) {
    BossEventPacket pkt;
    auto&           payload = static_cast<BossEventPacketPayload&>(pkt);

    payload.mBossID        = ActorUniqueID{bossId};
    payload.mEventType     = type;
    payload.mName          = Bedrock::Safety::RedactableString{title};
    payload.mHealthPercent = clamp01(progress01);
    payload.mColor         = static_cast<BossBarColor>(color);
    payload.mOverlay       = BossBarOverlay::Progress;
    return pkt;
}

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
    // 负值区段避开实体 ID 空间，减少与真实实体 BossBar 冲突
    int64_t id = -(int64_t)(store.size() + 1) - 1048576;
    store.emplace(key, id);
    return id;
}

void BossBar::show(Player& player, int64_t bossId, std::string const& title, float progress01, BarColor color) {
    auto pkt = makeBossPacket(bossId, BossEventUpdateType::Add, title, progress01, color);
    player.sendNetworkPacket(pkt);
}

void BossBar::update(Player& player, int64_t bossId, std::string const& title, float progress01, BarColor color) {
    auto namePkt = makeBossPacket(bossId, BossEventUpdateType::UpdateName, title, progress01, color);
    player.sendNetworkPacket(namePkt);

    auto pctPkt = makeBossPacket(bossId, BossEventUpdateType::UpdatePercent, title, progress01, color);
    player.sendNetworkPacket(pctPkt);

    auto stylePkt = makeBossPacket(bossId, BossEventUpdateType::UpdateStyle, title, progress01, color);
    player.sendNetworkPacket(stylePkt);
}

void BossBar::hide(Player& player, int64_t bossId) {
    auto pkt = makeBossPacket(bossId, BossEventUpdateType::Remove, {}, 0.f, BarColor::White);
    player.sendNetworkPacket(pkt);
}

void BossBar::forget(mce::UUID const& uuid) {
    std::lock_guard lock(mutex());
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
