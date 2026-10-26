#include "Visual.h"

#include "Compat.h"
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

/// BossBar 的 id 同时充当 ActorUniqueID 与 ActorRuntimeID，必须为正且避开真实实体 id 段
int64_t makeBossId(std::size_t index) {
    return (int64_t)(0x4000000000000000ULL | (uint64_t)(index & 0xFFFFULL));
}

/// 发送一条 BossEvent 包
void sendBossEvent(
    Player&             player,
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

    player.sendNetworkPacket(pkt);
}

/// 先给客户端造一个"看不见的玩家实体"，BossBar 必须挂在实体上才能渲染。
/// 注意：AddActorPacket 的构造函数未导出，必须像 LSE 一样走 MinecraftPackets 工厂。
void sendFakeBossActor(Player& player, int64_t bossId) {
    auto base = MinecraftPackets::createPacket(MinecraftPacketIds::AddActor);
    if (!base) {
        return;
    }
    auto* pkt = static_cast<AddActorPacket*>(base.get());
    if (!pkt) {
        return;
    }

    auto& payload = static_cast<AddActorPacketPayload&>(*pkt);

    payload.mEntityId  = ActorUniqueID{bossId};
    payload.mRuntimeId = ActorRuntimeID{(uint64)bossId};
    payload.mActorType = std::string{"player"};

    Vec3 pos = player.getFeetPos();
    pos.y    = (float)(worldMinYFor((int)player.getDimensionId()) - 64);
    payload.mPos = pos;

    // 其余字段（速度/旋转/属性/同步数据/连接）保持默认空值，与 LSE 的做法一致
    player.sendNetworkPacket(*pkt);
}

/// 与 LSE PlayerClass::setBossBar 完全一致的发送顺序：Remove -> AddActor -> Add
void pushBossBar(Player& player, int64_t bossId, std::string const& title, float progress01, BarColor color) {
    sendBossEvent(player, bossId, BossEventUpdateType::Remove, title, progress01, color);
    sendFakeBossActor(player, bossId);
    sendBossEvent(player, bossId, BossEventUpdateType::Add, title, progress01, color);
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
    pushBossBar(player, bossId, title, progress01, color);
}

void BossBar::update(Player& player, int64_t bossId, std::string const& title, float progress01, BarColor color) {
    // LSE 没有"增量更新"接口，Fcam 也是每秒重新 setBossBar 一次；这里保持一致
    pushBossBar(player, bossId, title, progress01, color);
}

void BossBar::hide(Player& player, int64_t bossId) {
    sendBossEvent(player, bossId, BossEventUpdateType::Remove, {}, 0.f, BarColor::White);
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
