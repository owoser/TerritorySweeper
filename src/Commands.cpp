#include "Commands.h"

#include "Config.h"
#include "Forms.h"
#include "Global.h"
#include "Selection.h"
#include "Task.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/Overload.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOriginType.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/PlayerCommandOrigin.h"
#include "mc/world/actor/player/Player.h"

#include <optional>
#include <string>
#include <vector>

namespace ts::command {

namespace {

std::string coordinateText(BlockPos const& pos) {
    return "(" + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z) + ")";
}

Player* asPlayer(CommandOrigin const& origin, CommandOutput& output) {
    if (origin.getOriginType() != CommandOriginType::Player) {
        output.error("该命令只能由玩家执行。");
        return nullptr;
    }
    auto* entity = static_cast<PlayerCommandOrigin const&>(origin).getEntity();
    if (!entity) {
        output.error("无法获取玩家实体。");
        return nullptr;
    }
    return static_cast<Player*>(entity);
}

/// 统一的前置校验：脚下领地在场 + 权限足够
std::optional<PlayerLand> requireLand(Player& player, CommandOutput& output) {
    auto land = resolveLandUnderPlayer(player);
    if (!land) {
        output.error("你不在任何领地内。");
        player.sendMessage("§c[领地清扫] 你不在任何领地内。");
        return std::nullopt;
    }
    if (!land->canManage()) {
        output.error("只有领地主人或管理员可以使用领地清扫功能。");
        player.sendMessage("§c[领地清扫] 只有领地主人或管理员可以使用该功能。");
        return std::nullopt;
    }
    return land;
}

void handleNew(Player& player, CommandOutput& output) {
    auto land = requireLand(player, output);
    if (!land) {
        return;
    }

    // 已在选点模式：先提示续选（这一步不能被名额检查挡住，否则选点途中再执行 /ts new
    // 会收到"名额已满"，看起来像选点失效）
    if (auto existing = SelectionManager::getInstance().get(player); existing && existing->active) {
        std::string a = existing->hasPos1 ? coordinateText(existing->pos1) : "未设置";
        std::string b = existing->hasPos2 ? coordinateText(existing->pos2) : "未设置";
        output.success("已在选点模式。");
        player.sendMessage("§e[领地清扫] 已在选点模式：点 A " + a + "，点 B " + b);
        player.sendMessage("§7如需重新开始或改回管理任务，请先 §f/ts cancel§7 取消本次选点。");
        return;
    }

    auto reject = TaskManager::getInstance().checkCreateAllowed(player, land->key);
    if (!reject.empty()) {
        output.error(reject);
        player.sendMessage("§c" + reject);
        return;
    }

    SelectionManager::getInstance().begin(player, *land);

    output.success("已进入选点模式。");
    player.sendMessage("§e[领地清扫] 已开启领地边框显示。请使用 §f木棍§e 点击第一个方块设置点 A。");
    player.sendMessage("§7（也可以使用 /ts pos1 与 /ts pos2 直接设置坐标；§f/ts cancel§7 可取消选点）");
}

void handleCancel(Player& player, CommandOutput& output) {
    auto state = SelectionManager::getInstance().get(player);
    if (!state || !state->active) {
        output.error("当前没有进行中的选点。");
        return;
    }
    SelectionManager::getInstance().cancel(player);
    output.success("已取消选点。");
    player.sendMessage("§7[领地清扫] 已取消本次选点。");
}

void setPoint(Player& player, CommandOutput& output, int which, BlockPos pos) {
    auto land = requireLand(player, output);
    if (!land) {
        return;
    }

    // 未点 A 就先执行指令时，自动开启选点
    if (!SelectionManager::getInstance().get(player)) {
        auto reject = TaskManager::getInstance().checkCreateAllowed(player, land->key);
        if (!reject.empty()) {
            output.error(reject);
            player.sendMessage("§c" + reject);
            return;
        }
        SelectionManager::getInstance().begin(player, *land);
    }

    if (!land->land.contains(pos)) {
        output.error("选区点超出领地范围。");
        player.sendMessage(
            "§c[领地清扫] 选区点超出领地范围。领地范围：X " + std::to_string(land->land.aabb.min.x) + "~"
            + std::to_string(land->land.aabb.max.x) + "  Z " + std::to_string(land->land.aabb.min.z) + "~"
            + std::to_string(land->land.aabb.max.z)
        );
        return;
    }

    if (!SelectionManager::getInstance().setPoint(player, which, pos)) {
        output.error("选点失败，请先使用 /ts new 开始任务。");
        return;
    }

    auto state = SelectionManager::getInstance().get(player);
    if (state) {
        SelectionManager::getInstance().refreshBox(player, *state);
    }

    std::string label = which == 1 ? "点 A" : "点 B";
    output.success(label + " 已设置。");
    player.sendMessage("§a[领地清扫] " + label + " 已设置 " + coordinateText(pos));

    if (state && state->complete()) {
        SelectionManager::getInstance().clear(player);
        player.sendMessage("§a[领地清扫] 选点完成，请使用 §f木棍§a 点击地面打开任务菜单。");
    } else if (which == 1) {
        player.sendMessage("§e请使用 §f木棍§e 点击第二个方块设置点 B（或 /ts pos2）。");
    } else {
        player.sendMessage("§e请使用 §f木棍§e 点击地面打开任务菜单（或 /ts ok）。");
    }
}

void handleOk(Player& player, CommandOutput& output) {
    auto land = requireLand(player, output);
    if (!land) {
        return;
    }

    auto state = SelectionManager::getInstance().get(player);
    if (!state || !state->complete()) {
        output.error("选区尚未完成，请先设置点 A 与点 B。");
        player.sendMessage("§c[领地清扫] 选区尚未完成，请先设置点 A 与点 B。");
        return;
    }

    SelectionManager::getInstance().clear(player);
    form::openDirection(player, *land, *state);
    output.success("已打开任务菜单。");
}

/// 该领地任务的简要列表（多任务提示用）
std::string describeLandTasks(std::vector<SweepTask*> const& tasks) {
    std::string out;
    for (auto* task : tasks) {
        out += " - #" + std::to_string(task->id) + " " + toString(task->state) + " | 进度 "
             + percentText(task->progressPercent()) + "\n";
    }
    return out;
}

void handleStart(Player& player, CommandOutput& output) {
    auto land = requireLand(player, output);
    if (!land) {
        return;
    }

    auto& manager = TaskManager::getInstance();
    auto  tasks   = manager.tasksOfLand(land->key);
    if (tasks.empty()) {
        output.error("本领地没有可继续的清扫任务。");
        return;
    }

    int resumed = 0;
    for (auto* task : tasks) {
        if (task->state == TaskState::Paused && manager.resumeTask(task->id)) {
            ++resumed;
        }
    }
    if (resumed == 0) {
        output.error("本领地的清扫任务都在运行中。");
        return;
    }
    output.success("已继续 " + std::to_string(resumed) + " 个任务。");
    player.sendMessage("§a[领地清扫] 已继续 " + std::to_string(resumed) + " 个任务。");
}

void handleStop(Player& player, CommandOutput& output) {
    auto land = requireLand(player, output);
    if (!land) {
        return;
    }

    auto& manager = TaskManager::getInstance();
    auto  tasks   = manager.tasksOfLand(land->key);
    if (tasks.empty()) {
        output.error("本领地没有正在运行的清扫任务。");
        return;
    }

    int paused = 0;
    for (auto* task : tasks) {
        if (task->state == TaskState::Running) {
            manager.pauseTask(task->id, "领主手动暂停");
            ++paused;
        }
    }
    if (paused == 0) {
        output.error("本领地的任务已全部处于暂停状态。");
        return;
    }
    output.success("已暂停 " + std::to_string(paused) + " 个任务。");
    player.sendMessage("§e[领地清扫] 已暂停 " + std::to_string(paused) + " 个任务。");
}

void handleKill(Player& player, CommandOutput& output, std::optional<uint64_t> taskId) {
    auto land = requireLand(player, output);
    if (!land) {
        return;
    }

    auto& manager = TaskManager::getInstance();
    auto  tasks   = manager.tasksOfLand(land->key);
    if (tasks.empty()) {
        output.error("本领地没有可终止的清扫任务。");
        return;
    }

    if (!taskId) {
        if (tasks.size() > 1) {
            std::string detail = "本领地有 " + std::to_string(tasks.size())
                               + " 个清扫任务，请使用 /ts kill <任务ID> 指定要终止的任务：\n"
                               + describeLandTasks(tasks);
            output.error(detail);
            player.sendMessage("§c[领地清扫] " + detail);
            return;
        }
        taskId = tasks.front()->id;
    }

    auto* target = manager.taskById(*taskId);
    if (target == nullptr || target->plan.landId != land->key) {
        output.error("任务 #" + std::to_string(*taskId) + " 不属于当前领地。");
        player.sendMessage("§c[领地清扫] 任务 #" + std::to_string(*taskId) + " 不属于当前领地。");
        return;
    }

    uint64_t const id = target->id;
    manager.killTask(id, "领主通过 /ts kill 终止");
    output.success("任务 #" + std::to_string(id) + " 已终止。");
    player.sendMessage("§c[领地清扫] 任务 #" + std::to_string(id) + " 已终止。");
}

} // namespace

/// 指令参数结构体必须具有外部链接（LL3 的反射基于 boost::pfr）
struct PosArgs {
    int x;
    int y;
    int z;
};

/// /ts kill <id>
struct IdArgs {
    int id;
};

void registerCommands() {
    auto& registrar = ll::command::CommandRegistrar::getInstance(false);
    auto& cmd = registrar.getOrCreateCommand("ts", "领地分层清扫", CommandPermissionLevel::Any);

    // /ts
    cmd.overload<ll::command::EmptyParam>().execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            auto* player = asPlayer(origin, output);
            if (!player) {
                return;
            }
            player->sendMessage(
                "§e===== 领地清扫指令 =====\n"
                "§f/ts new§7 创建清扫任务并开始选点\n"
                "§f/ts pos1 [x y z]§7 设置点 A\n"
                "§f/ts pos2 [x y z]§7 设置点 B\n"
                "§f/ts ok§7 确认选区并打开任务菜单\n"
                "§f/ts cancel§7 取消当前选点\n"
                "§f/ts list§7 查看任务与账单列表\n"
                "§f/ts start§7 继续本领地的全部任务\n"
                "§f/ts stop§7 暂停本领地的全部任务\n"
                "§f/ts kill [任务ID]§7 终止任务（本领地有多个任务时必须带 ID）\n"
                "§7也可以手持木棍点击方块与地面进行操作。"
            );
            output.success("已输出帮助信息。");
        }
    );

    // /ts new
    cmd.overload<ll::command::EmptyParam>().text("new").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                handleNew(*player, output);
            }
        }
    );

    // /ts pos1 / /ts pos2（脚下方块）
    cmd.overload<ll::command::EmptyParam>().text("pos1").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                setPoint(*player, output, 1, feetBlock(*player));
            }
        }
    );
    cmd.overload<ll::command::EmptyParam>().text("pos2").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                setPoint(*player, output, 2, feetBlock(*player));
            }
        }
    );

    // /ts pos1 <x> <y> <z>
    cmd.overload<PosArgs>().text("pos1").required("x").required("y").required("z").execute(
        [](CommandOrigin const& origin, CommandOutput& output, PosArgs const& args, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                setPoint(*player, output, 1, BlockPos{args.x, args.y, args.z});
            }
        }
    );
    cmd.overload<PosArgs>().text("pos2").required("x").required("y").required("z").execute(
        [](CommandOrigin const& origin, CommandOutput& output, PosArgs const& args, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                setPoint(*player, output, 2, BlockPos{args.x, args.y, args.z});
            }
        }
    );

    // /ts ok
    cmd.overload<ll::command::EmptyParam>().text("ok").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                handleOk(*player, output);
            }
        }
    );

    // /ts cancel
    cmd.overload<ll::command::EmptyParam>().text("cancel").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                handleCancel(*player, output);
            }
        }
    );

    // /ts list
    cmd.overload<ll::command::EmptyParam>().text("list").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                form::openTaskList(*player);
                output.success("已打开任务列表。");
            }
        }
    );

    // /ts start / stop / kill
    cmd.overload<ll::command::EmptyParam>().text("start").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                handleStart(*player, output);
            }
        }
    );
    cmd.overload<ll::command::EmptyParam>().text("stop").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                handleStop(*player, output);
            }
        }
    );
    cmd.overload<ll::command::EmptyParam>().text("kill").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ll::command::EmptyParam const&, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                handleKill(*player, output, std::nullopt);
            }
        }
    );
    cmd.overload<IdArgs>().text("kill").required("id").execute(
        [](CommandOrigin const& origin, CommandOutput& output, IdArgs const& args, ::Command const&) {
            if (auto* player = asPlayer(origin, output); player) {
                if (args.id <= 0) {
                    output.error("任务 ID 必须是正整数。");
                    return;
                }
                handleKill(*player, output, (uint64_t)args.id);
            }
        }
    );

    log::get().info("已注册 /ts 指令。");
}

} // namespace ts::command
