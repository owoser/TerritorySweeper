#include "LiquidGuard.h"

#include "Config.h"
#include "Global.h"
#include "Task.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/world/BlockChangedEvent.h"

#include "mc/util/BlockUtils.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"

#include <memory>
#include <vector>
#include <string_view>

namespace ts::liquid {

namespace {

std::vector<std::shared_ptr<ll::event::ListenerBase>> gListeners;

/// 本线程是否正在执行还原（避免 setBlock 再次触发事件造成递归）
thread_local bool gReverting = false;

bool isLiquidTypeName(std::string_view name) {
    return name == "minecraft:water" || name == "minecraft:flowing_water" || name == "minecraft:lava"
        || name == "minecraft:flowing_lava";
}

bool isLiquidBlock(Block const& block) {
    return isLiquidTypeName(block.getTypeName()) || BlockUtils::isLiquidSource(block)
        || BlockUtils::isFullFlowingLiquid(block);
}

Block const* airBlock() {
    static Block const* ptr = []() -> Block const* {
        auto ref = Block::tryGetFromRegistry(HashedString{"minecraft:air"});
        return ref.has_value() ? &*ref : nullptr;
    }();
    return ptr;
}

void onBlockChanged(ll::event::world::BlockChangedEvent& event) {
    if (gReverting) {
        return;
    }
    if (!ConfigManager::getInstance().get().blockLiquidInflow) {
        return;
    }

    Block const& newBlock = event.newBlock();
    if (!isLiquidBlock(newBlock)) {
        return;
    }

    BlockSource& source = event.blockSource();
    int const    dimId  = static_cast<int>(source.getDimensionId());
    BlockPos const& pos = event.pos();

    if (!TaskManager::getInstance().isInsideActiveTask(dimId, pos)) {
        return;
    }

    Block const* air = airBlock();
    if (!air) {
        return;
    }

    gReverting = true;
    BlockChangeContext context{};
    source.setBlock(pos, *air, 3, nullptr, context);
    gReverting = false;

    TS_DBG("[liquid] 拦截流入 ({}, {}, {}) dim={}", pos.x, pos.y, pos.z, dimId);
}

} // namespace

void registerListener() {
    if (!gListeners.empty()) {
        return;
    }
    gListeners.emplace_back(ll::event::EventBus::getInstance().emplaceListener<ll::event::world::BlockChangedEvent>(
        [](ll::event::world::BlockChangedEvent& event) {
            try {
                onBlockChanged(event);
            } catch (std::exception const& e) {
                log::get().error("液体拦截处理异常: {}", e.what());
            } catch (...) {
                log::get().error("液体拦截处理发生未知异常");
            }
        }
    ));
}

void unregisterListener() { gListeners.clear(); }

} // namespace ts::liquid
