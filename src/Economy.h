#pragma once

/**
 * 经济服务
 *
 * 支持两种后端，由 config.json 的 economy_type 切换：
 *   - LLMoney   （默认）：调用 LegacyMoney 插件的 LLMoney_Get / LLMoney_Reduce（按 XUID 结算）
 *   - Scoreboard：使用 BDS 原生计分板（按玩家名结算，目标名由 scoreboard_objective 指定）
 *
 * 所有扣款前都会显式校验余额，只有余额充足才执行扣款，避免出现负余额。
 */

#include "mc/platform/UUID.h"

#include <cstdint>
#include <string>

namespace ts::economy {

struct ChargeTarget {
    std::string xuid;
    std::string name;
    mce::UUID   uuid{};

    [[nodiscard]] bool valid() const { return !name.empty(); }
};

class EconomyService {
public:
    static EconomyService& getInstance();

    /// 依据当前配置选择后端；返回后端是否可用
    bool init();

    [[nodiscard]] bool        available() const;
    [[nodiscard]] bool        isScoreboard() const;
    [[nodiscard]] std::string describe() const;

    /// 读取余额（不修改）
    [[nodiscard]] bool balance(ChargeTarget const& target, int64_t& out) const;

    /// 余额是否充足
    [[nodiscard]] bool hasEnough(ChargeTarget const& target, int64_t amount) const;

    /// 原子扣款：先校验余额，充足才扣；返回是否扣款成功
    [[nodiscard]] bool reduce(ChargeTarget const& target, int64_t amount) const;

private:
    EconomyService() = default;

    enum class Kind {
        None,
        LLMoney,
        Scoreboard,
    };

    [[nodiscard]] bool scoreboardUsable() const;

    Kind        mKind{Kind::None};
    std::string mObjective;
};

} // namespace ts::economy
