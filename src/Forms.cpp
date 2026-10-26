#include "Forms.h"

#include "Config.h"
#include "Economy.h"
#include "Global.h"
#include "Visual.h"

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/ModalForm.h"
#include "ll/api/form/SimpleForm.h"

#include "mc/world/actor/player/Player.h"

#include <cmath>
#include <ctime>
#include <string>

namespace ts::form {

namespace {

using ll::form::CustomForm;
using ll::form::ModalForm;
using ll::form::SimpleForm;

std::string money(int64_t v) { return std::to_string(v) + " " + ConfigManager::getInstance().get().moneyname; }

/// 计算分块配置
struct ChunkPlan {
    bool chunked{false};
    int  w{10};
    int  d{10};
    int  interval{2};
};

ChunkPlan chunkPlanFor(int width, int depth) {
    auto const& cfg = ConfigManager::getInstance().get();
    ChunkPlan  plan;

    plan.w        = cfg.chunkMatrixWidth;
    plan.d        = cfg.chunkMatrixDepth;
    plan.interval = cfg.chunkIntervalSeconds;
    plan.chunked  = cfg.chunkedCleanEnabled && width * depth > cfg.chunkedMinArea;
    return plan;
}

/// 组装完整的 SweepPlan（不含层范围）
SweepPlan buildPlanBase(Player& player, PlayerLand const& land, SelectionState const& sel, SweepDirection dir) {
    auto const& cfg = ConfigManager::getInstance().get();

    SweepPlan plan;
    plan.dimId     = sel.dimId;
    plan.minX      = std::min(sel.pos1.x, sel.pos2.x);
    plan.maxX      = std::max(sel.pos1.x, sel.pos2.x);
    plan.minZ      = std::min(sel.pos1.z, sel.pos2.z);
    plan.maxZ      = std::max(sel.pos1.z, sel.pos2.z);
    plan.direction = dir;
    plan.startY    = (int)std::floor(player.getFeetPos().y);

    plan.landId   = land.land.id;
    plan.landName = land.land.name;

    auto const& level = cfg.matchPriceLevel(plan.width(), plan.depth());
    plan.levelname    = level.levelname;
    plan.unitPrice    = level.pricePerLayer;

    auto chunk        = chunkPlanFor(plan.width(), plan.depth());
    plan.chunked      = chunk.chunked;
    plan.chunkWidth   = chunk.w;
    plan.chunkDepth   = chunk.d;
    plan.chunkInterval = std::max(0, chunk.interval);
    plan.layerCountdown = std::max(1, cfg.layerCountdownSeconds);

    return plan;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 方向
// ---------------------------------------------------------------------------

void openDirection(Player& player, PlayerLand const& land, SelectionState const& selection) {
    auto& logger = log::get();
    logger.debug("为 {} 打开方向选择表单（领地#{}）", player.getRealName(), land.land.id);

    std::string content =
        "请选择本次清扫任务朝向。\n"
        "向上清理为无任何方块的空气层截止。\n"
        "向下清理为最后一层基岩层截止。\n"
        "中途可使用木棍点击打开任务菜单进行暂停或者终止任务。\n"
        "注意：清理始发地为玩家脚下坐标。\n"
        "\n选区范围：X " + std::to_string(selection.pos1.x) + "~" + std::to_string(selection.pos2.x) + "  Z "
        + std::to_string(selection.pos1.z) + "~" + std::to_string(selection.pos2.z);

    ModalForm form("领地清扫任务", content, "§a向上清理", "§b向下清理");

    auto landCopy      = land;
    auto selectionCopy = selection;

    form.sendTo(player, [landCopy, selectionCopy](Player& p, ll::form::ModalFormResult result, ll::form::FormCancelReason) {
        if (!result) {
            p.sendMessage("§7已取消清扫任务。");
            return;
        }
        auto direction = (*result == ll::form::ModalFormSelectedButton::Upper) ? SweepDirection::Up : SweepDirection::Down;
        openLayerInput(p, direction, landCopy, selectionCopy);
    });
}

// ---------------------------------------------------------------------------
// 2. 层数
// ---------------------------------------------------------------------------

void openLayerInput(Player& player, SweepDirection direction, PlayerLand const& land, SelectionState const& selection) {
    CustomForm form("清扫任务配置");
    form.appendLabel("§e当前方向：" + std::string(toString(direction)));
    form.appendLabel("§e起始层：玩家脚下坐标");
    form.appendInput("layers", "请选择清理层数，留空则系统自动计算", "输入大于 0 的数字，或留空");

    auto landCopy      = land;
    auto selectionCopy = selection;

    form.sendTo(
        player,
        [landCopy, selectionCopy, direction](
            Player&                 p,
            ll::form::CustomFormResult const& result,
            ll::form::FormCancelReason
        ) {
            if (!result) {
                p.sendMessage("§7已取消清扫任务。");
                return;
            }

            int manual = 0;
            if (auto it = result->find("layers"); it != result->end()) {
                if (auto const* text = std::get_if<std::string>(&it->second); text && !text->empty()) {
                    try {
                        manual = std::stoi(*text);
                    } catch (...) {
                        manual = 0;
                    }
                }
            }
            if (manual < 0) {
                manual = 0;
            }

            auto plan = buildPlanBase(p, landCopy, selectionCopy, direction);

            auto range = computeLayerRange(p, plan, direction, manual);
            if (!range) {
                p.sendMessage("§c未找到可清理的层，请调整选区或手动输入层数。");
                return;
            }

            plan.minY           = range->minY;
            plan.maxY           = range->maxY;
            plan.startY         = range->startY;
            plan.layerCount     = range->count;
            plan.layerCountAuto = range->autoComputed;
            plan.totalPrice     = plan.unitPrice * (int64_t)plan.layerCount;

            openCostDetail(p, plan);
        }
    );
}

// ---------------------------------------------------------------------------
// 3. 费用详情
// ---------------------------------------------------------------------------

void openCostDetail(Player& player, SweepPlan plan) {
    auto const& cfg = ConfigManager::getInstance().get();

    std::string matrix = plan.chunked
                           ? (std::to_string(plan.chunkWidth) + "*" + std::to_string(plan.chunkDepth))
                           : "不分块";

    std::string content =
        "收费等级：" + plan.levelname + "\n"
        "经济系统：" + economy::EconomyService::getInstance().describe() + "\n"
        "是否分块清理：" + std::string(plan.chunked ? "是" : "否") + "\n"
        "分块矩阵：" + matrix + "\n"
        "清理方向：" + std::string(toString(plan.direction)) + "\n"
        "层数：" + std::to_string(plan.layerCount) + " 层"
        + (plan.layerCountAuto ? "（系统自动计算）" : "（手动指定）") + "\n"
        "Y 范围：" + std::to_string(plan.minY) + " ~ " + std::to_string(plan.maxY) + "\n"
        "每层单价：" + money(plan.unitPrice) + "\n"
        "合计：§e" + money(plan.totalPrice);

    if (cfg.exceedsLargest(plan.width(), plan.depth())) {
        content += "\n§c注意：选区超出已配置的最大规格，已按最大规格计费。";
    }

    ModalForm form("清扫任务费用", content, "确定", "返回");
    form.sendTo(
        player,
        [plan](Player& p, ll::form::ModalFormResult result, ll::form::FormCancelReason) {
            if (!result) {
                p.sendMessage("§7已取消清扫任务。");
                return;
            }
            openPayment(p, plan);
        }
    );
}

// ---------------------------------------------------------------------------
// 4. 二次确认 / 支付
// ---------------------------------------------------------------------------

void openPayment(Player& player, SweepPlan plan) {
    auto& economyService = economy::EconomyService::getInstance();

    economy::ChargeTarget target;
    target.xuid = player.getXuid();
    target.name = player.getRealName();
    target.uuid = player.getUuid();

    int64_t balance = 0;
    bool    readOk  = economyService.balance(target, balance);
    bool    enough  = readOk && balance >= plan.totalPrice;

    std::string content =
        "应付金额：" + money(plan.totalPrice) + "\n"
        "玩家当前余额：" + (readOk ? money(balance) : std::string("§c读取失败")) + "\n"
        "支付后端：" + economyService.describe() + "\n"
        "状态：" + (enough ? "§a余额充足" : "§c余额不足，无法立即支付") + "\n\n"
        "§7选择『挂起账单』可保存本次配置，稍后回到本领地使用木棍点击即可继续支付。";

    SimpleForm form("清扫费用二次确认");
    form.setContent(content);

    auto planCopy = plan;

    form.appendButton("§a立即支付", [planCopy, target](Player& p) {
        auto& service = economy::EconomyService::getInstance();

        if (!service.available()) {
            p.sendMessage("§c支付失败：经济系统不可用（" + service.describe() + "）。");
            openPayment(p, planCopy);
            return;
        }
        if (!service.hasEnough(target, planCopy.totalPrice)) {
            p.sendMessage("§c余额不足，无法立即支付，请充值后重试。");
            openPayment(p, planCopy);
            return;
        }
        if (!service.reduce(target, planCopy.totalPrice)) {
            p.sendMessage("§c扣款失败，请检查经济系统或联系管理员。");
            openPayment(p, planCopy);
            return;
        }

        TaskManager::getInstance().dropBill(planCopy.landId);
        auto* task = TaskManager::getInstance().startTask(p, planCopy, true);
        if (!task) {
            p.sendMessage("§c任务创建失败，费用已扣除，请联系管理员处理。");
            return;
        }

        SelectionManager::getInstance().clear(p);
        p.sendMessage(
            "§a[领地清扫] 已支付 " + std::to_string(planCopy.totalPrice) + " "
            + ConfigManager::getInstance().get().moneyname + "，任务开始，首轮倒计时 "
            + std::to_string(planCopy.layerCountdown) + " 秒。"
        );
    });

    form.appendButton("§6挂起账单", [planCopy, target](Player& p) {
        PendingBill bill;
        bill.ownerName = target.name;
        bill.ownerXuid = target.xuid;
        bill.ownerUuid = target.uuid;
        bill.plan      = planCopy;
        bill.createdAt = (int64_t)std::time(nullptr);

        TaskManager::getInstance().storeBill(bill);
        SelectionManager::getInstance().clear(p);
        p.sendMessage("§6[领地清扫] 已挂起账单，回到本领地使用木棍点击地面即可继续支付。");
    });

    form.appendButton("§7关闭任务", [](Player& p) {
        SelectionManager::getInstance().clear(p);
        p.sendMessage("§7已关闭本次清扫任务，未产生任何费用。");
    });

    form.sendTo(player);
}

// ---------------------------------------------------------------------------
// 5. 任务管理
// ---------------------------------------------------------------------------

void openManage(Player& player, SweepTask& task) {
    std::string content =
        "任务 #" + std::to_string(task.id) + "\n"
        "方向：" + toString(task.plan.direction) + "\n"
        "进度：" + std::to_string(task.progressPercent()) + "%  ("
        + std::to_string(task.layersDone) + "/" + std::to_string(task.plan.layerCount) + " 层)\n"
        "状态：" + toString(task.state) + "\n"
        "范围：X " + std::to_string(task.plan.minX) + "~" + std::to_string(task.plan.maxX) + "  Z "
        + std::to_string(task.plan.minZ) + "~" + std::to_string(task.plan.maxZ) + "\n"
        + (task.state == TaskState::Running
               ? "本层剩余：" + std::to_string(task.countdown) + " 秒"
               : "已暂停：" + std::to_string(task.pausedSeconds) + " 秒（"
                     + std::to_string(ConfigManager::getInstance().get().pauseAutoCancelSeconds)
                     + " 秒后自动终止）");

    SimpleForm form("领地清扫管理");
    form.setContent(content);

    uint64_t id = task.id;

    if (task.state == TaskState::Running) {
        form.appendButton("§e暂停任务", [id](Player& p) {
            TaskManager::getInstance().pauseTask(id, "领主手动暂停");
            p.sendMessage("§e[领地清扫] 任务已暂停。");
        });
    } else {
        form.appendButton("§a继续任务", [id](Player& p) {
            if (TaskManager::getInstance().resumeTask(id)) {
                p.sendMessage("§a[领地清扫] 任务已继续。");
            } else {
                p.sendMessage("§c[领地清扫] 任务不存在或已结束。");
            }
        });
    }

    form.appendButton("§c停止任务", [id](Player& p) { openKillConfirm(p, id); });
    form.appendButton("§7关闭菜单", [](Player&) {});

    form.sendTo(player);
}

void openKillConfirm(Player& player, uint64_t taskId) {
    ModalForm form(
        "终止清扫任务",
        "确定要终止该清扫任务吗？\n§c终止后本次已支付费用不会退还，且已清理区域不会恢复。",
        "§c确定终止",
        "§7返回"
    );
    form.sendTo(player, [taskId](Player& p, ll::form::ModalFormResult result, ll::form::FormCancelReason) {
        if (!result || *result != ll::form::ModalFormSelectedButton::Upper) {
            p.sendMessage("§7已取消终止操作。");
            return;
        }
        TaskManager::getInstance().killTask(taskId, "领主手动终止");
    });
}

// ---------------------------------------------------------------------------
// 6. 待支付账单
// ---------------------------------------------------------------------------

void openBill(Player& player, PendingBill const& bill) {
    auto plan = bill.plan;

    std::string content =
        "§6本领地存在待支付账单\n"
        "收费等级：" + plan.levelname + "\n"
        "层数：" + std::to_string(plan.layerCount) + " 层\n"
        "方向：" + toString(plan.direction) + "\n"
        "范围：X " + std::to_string(plan.minX) + "~" + std::to_string(plan.maxX) + "  Z "
        + std::to_string(plan.minZ) + "~" + std::to_string(plan.maxZ) + "\n"
        "应付金额：§e" + money(plan.totalPrice);

    SimpleForm form("领地清扫账单");
    form.setContent(content);

    form.appendButton("§a立即支付", [plan](Player& p) {
        auto& service = economy::EconomyService::getInstance();

        economy::ChargeTarget target;
        target.xuid = p.getXuid();
        target.name = p.getRealName();
        target.uuid = p.getUuid();

        if (!service.hasEnough(target, plan.totalPrice)) {
            p.sendMessage("§c余额不足，无法支付该账单。");
            openPayment(p, plan);
            return;
        }
        if (!service.reduce(target, plan.totalPrice)) {
            p.sendMessage("§c扣款失败，请稍后重试。");
            return;
        }

        TaskManager::getInstance().dropBill(plan.landId);
        auto* task = TaskManager::getInstance().startTask(p, plan, true);
        if (!task) {
            p.sendMessage("§c任务创建失败，费用已扣除，请联系管理员处理。");
            return;
        }
        p.sendMessage("§a[领地清扫] 账单已支付，任务开始。");
    });

    form.appendButton("§6保留账单", [](Player& p) { p.sendMessage("§7账单已保留，稍后可再次支付。"); });

    form.appendButton("§c关闭账单", [plan](Player& p) {
        TaskManager::getInstance().dropBill(plan.landId);
        SelectionManager::getInstance().clear(p);
        p.sendMessage("§7账单已关闭。");
    });

    form.sendTo(player);
}

// ---------------------------------------------------------------------------
// 7. 任务列表
// ---------------------------------------------------------------------------

void openTaskList(Player& player) {
    auto&       manager = TaskManager::getInstance();
    std::string content = "§e进行中的任务§r\n" + manager.listTasks() + "\n§e待支付账单§r\n" + manager.listBills();

    SimpleForm form("领地清扫任务列表");
    form.setContent(content);
    form.appendButton("§7关闭", [](Player&) {});
    form.sendTo(player);
}

// ---------------------------------------------------------------------------
// 8. 木棍智能菜单
// ---------------------------------------------------------------------------

void openSmartMenu(Player& player, PlayerLand const& land) {
    auto& manager = TaskManager::getInstance();

    if (auto* task = manager.taskOfLand(land.land.id); task) {
        openManage(player, *task);
        return;
    }

    if (auto const* bill = manager.billOfLand(land.land.id); bill) {
        openBill(player, *bill);
        return;
    }

    auto selection = SelectionManager::getInstance().get(player);
    if (selection && selection->complete() && selection->landId == land.land.id) {
        openDirection(player, land, *selection);
        return;
    }

    player.sendMessage("§e[领地清扫] 请先用 §f木棍§e 依次点击两个方块选择点 A 与点 B，再点击地面打开任务菜单。");
}

} // namespace ts::form
