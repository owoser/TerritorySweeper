#pragma once

/**
 * 液体流入拦截
 *
 * 任务运行期间，若液体（水/岩浆）被放入清理范围内的格子，立即还原成空气。
 * 效果等同 PLand 的 allowLiquidFlow：领地外的水不会灌进领地。
 *
 * 说明：LeviLamina 本体没有"液体流动前"事件，这里用 BlockChangedEvent 事后还原，
 * 功能一致，代价是液体可能闪一帧。
 */
namespace ts::liquid {

void registerListener();
void unregisterListener();

} // namespace ts::liquid
