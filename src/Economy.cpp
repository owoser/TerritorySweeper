#include "Economy.h"

#include "Compat.h"
#include "Config.h"
#include "Global.h"

#include "ll/api/service/Bedrock.h"

#include "mc/world/level/Level.h"
#include "mc/world/scores/Objective.h"
#include "mc/world/scores/PlayerScoreSetFunction.h"
#include "mc/world/scores/Scoreboard.h"
#include "mc/world/scores/ScoreboardOperationResult.h"

namespace ts::economy {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) {
        c = (char)std::tolower((unsigned char)c);
    }
    return s;
}

} // namespace

EconomyService& EconomyService::getInstance() {
    static EconomyService instance;
    return instance;
}

bool EconomyService::init() {
    auto const& cfg = ConfigManager::getInstance().get();

    if (lower(cfg.economyType) == "scoreboard") {
        mKind      = Kind::Scoreboard;
        mObjective = cfg.scoreboardObjective;
        if (mObjective.empty()) {
            log::get().error("经济系统配置为 Scoreboard，但 scoreboard_objective 为空，扣款将失败。");
            return false;
        }
        if (!scoreboardUsable()) {
            log::get().warn(
                "计分板目标 '{}' 当前不存在，请使用 /scoreboard objectives add {} dummy \"{}\" 预先创建。"
                "（任务创建时会再次校验）",
                mObjective,
                mObjective,
                cfg.moneyname
            );
        }
        log::get().info("经济系统后端: 计分板 (objective = {})", mObjective);
        return true;
    }

    mKind = Kind::LLMoney;
    if (!compat::LLMoneyCompat::getInstance().available()) {
        log::get().error("经济系统配置为 LLMoney，但 LegacyMoney 插件不可用，将无法扣款。");
        return false;
    }
    log::get().info("经济系统后端: LLMoney (LegacyMoney {})", compat::LLMoneyCompat::getInstance().version());
    return true;
}

bool EconomyService::available() const {
    switch (mKind) {
    case Kind::LLMoney:
        return compat::LLMoneyCompat::getInstance().available();
    case Kind::Scoreboard:
        return scoreboardUsable();
    default:
        return false;
    }
}

bool EconomyService::isScoreboard() const { return mKind == Kind::Scoreboard; }

std::string EconomyService::describe() const {
    switch (mKind) {
    case Kind::LLMoney:
        return "LLMoney";
    case Kind::Scoreboard:
        return "计分板(" + mObjective + ")";
    default:
        return "未配置";
    }
}

bool EconomyService::scoreboardUsable() const {
    if (mObjective.empty()) {
        return false;
    }
    auto level = ll::service::getLevel();
    if (!level) {
        return false;
    }
    return level->getScoreboard().getObjective(mObjective) != nullptr;
}

bool EconomyService::balance(ChargeTarget const& target, int64_t& out) const {
    switch (mKind) {
    case Kind::LLMoney: {
        if (!compat::LLMoneyCompat::getInstance().get(target.xuid, out)) {
            return false;
        }
        return true;
    }
    case Kind::Scoreboard: {
        auto level = ll::service::getLevel();
        if (!level || !target.valid()) {
            return false;
        }
        auto& board    = level->getScoreboard();
        auto* objective = board.getObjective(mObjective);
        if (!objective) {
            return false;
        }
        auto score = objective->getPlayerScore(board.getScoreboardId(target.name));
        out        = score.mValid ? score.mValue : 0;
        return true;
    }
    default:
        return false;
    }
}

bool EconomyService::hasEnough(ChargeTarget const& target, int64_t amount) const {
    if (amount <= 0) {
        return true;
    }
    int64_t bal = 0;
    if (!balance(target, bal)) {
        return false;
    }
    return bal >= amount;
}

bool EconomyService::reduce(ChargeTarget const& target, int64_t amount) const {
    if (amount <= 0) {
        return true;
    }

    int64_t bal = 0;
    if (!balance(target, bal)) {
        log::get().warn("扣款失败：无法读取玩家 {} 的余额（后端 {}）", target.name, describe());
        return false;
    }
    if (bal < amount) {
        return false;
    }

    switch (mKind) {
    case Kind::LLMoney:
        return compat::LLMoneyCompat::getInstance().reduce(target.xuid, amount);

    case Kind::Scoreboard: {
        auto level = ll::service::getLevel();
        if (!level || !target.valid()) {
            return false;
        }
        auto& board     = level->getScoreboard();
        auto* objective = board.getObjective(mObjective);
        if (!objective) {
            return false;
        }
        ScoreboardOperationResult result;
        board.modifyPlayerScore(
            result,
            board.getScoreboardId(target.name),
            *objective,
            (int)amount,
            PlayerScoreSetFunction::Subtract
        );
        // 扣款后再校验一次，确保没有扣成负数
        int64_t after = 0;
        return balance(target, after) && after >= 0;
    }

    default:
        return false;
    }
}

} // namespace ts::economy
