# TerritorySweeper 未发布迭代记录 (UNRELEASED.md)

本文件记录自上次正式发版 **`v1.3.0`** 之后的所有代码提交、功能迭代、问题排查根因与性能优化。
**本文件提交至 Git 仓库公开跟踪，用于抵抗 AI 上下文压缩丢失历史事实，并作为下一次正式发版 Release Notes 的事实来源。**

---

## 📌 当前版本迭代标定

- **上个发版版本**：`v1.3.0`（详见 `release-notes/v1.3.0.md`）
- **配置系统版本**：`config_version: 5`
- **数据库 schema 版本**：`4`
- **构建基线**：LeviLamina 26.60.8（clang-cl 23 / MSVC 14.44 / xmake 3.x）

---

## 📝 提交历史明细（v1.3.0 之后）

### 2026-11-20 · 构建依赖：移除自建预编译 SDK，回归官方源码构建

**背景**：`xmake-repo-extra` 的预编译 SDK 原本是为绕开 clang 19 下 LeviLamina 源码构建失败
（`src/mc/server/commands/MinecraftCommands.h` 报 `invalid application of 'sizeof' to an incomplete type`）。
clang-cl 23 下该问题已消失，且上游 release 资产只含 `LeviLamina.dll/.pdb/manifest.json`（从不提供 include/lib），
自建 SDK 需要双镜像 pre-release + sha256 维护，属于额外负担，故整体移除。

**改动**
- `xmake.lua`：删除 `add_repositories("ts-extra xmake-repo-extra")`，仅保留官方
  `add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")`；注释改为说明
  LeviLamina 26.60.x 由 xmake 从上游源码 tag 构建安装。
- 删除 `xmake-repo-extra/packages/l/levilamina/xmake.lua`、`tools/build-sdk.ps1`、`tools/build-sdk-from-release.ps1`。
- `.github/workflows/release.yml`：actions/cache key 由 `hashFiles('xmake.lua', 'xmake-repo-extra/**/xmake.lua')`
  简化为 `hashFiles('xmake.lua')`，job 注释说明首次构建需从源码编译 LeviLamina（耗时十几分钟），产物落在
  `~/AppData/Local/.xmake` 包缓存并随缓存复用。

**验证**：`xmake f -c -y --toolchain=clang-cl` + `xmake -y` → `[100%]: build ok`。
配置阶段 191.7s（含 clone 与从源码构建 LeviLamina 26.60.8：`bin/LeviLamina.dll` 5,005,312 B、
`bin/PreLoader.dll` 10,743,296 B、`lib/LeviLamina.lib` 546,626 B），编译链接 22.4s，总计 215.2s。
安装目录哈希 `1063cfd5df854512934dee6527023253`，包定义来自 levimc-repo commit `fdab2d5`。

---

## ⚙️ 新增/变更配置项汇总表（对比 v1.3.0）

| 配置项 | 类型 | 默认值 | 作用说明 |
| :--- | :--- | :--- | :--- |
| _（暂无新增配置项）_ | - | - | - |
