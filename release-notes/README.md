# release-notes/

本目录存放**正式发版用的发行说明**，与仓库内其余文档的关系：

| 文件 | 定位 | 维护时机 |
| :--- | :--- | :--- |
| `UNRELEASED.md` | 未发布迭代日志（提交级事实库，抵抗上下文压缩） | 每次重要迭代后增量追加 |
| `release-notes/<tag>.md` | 某个正式版本的**面向用户的发行说明** | 发版前，从 `UNRELEASED.md` 汇总撰写 |
| `.github/workflows/release.yml` | 读取本目录并按 tag 自动发版 | — |

## 约定

1. 文件名必须与 tag 完全一致，例如 `v1.2.0` → `release-notes/v1.2.0.md`；
2. 内容以 `UNRELEASED.md` 为事实来源整理（功能、性能、修复、配置项变更、安装说明），
   剔除开发调试细节，文风与历史 Release 保持一致；
3. **发布前必须先提交该文件**，再推送 tag，否则 CI 会回退到 GitHub 自动生成的发行说明，
   并在 Actions 日志里打 `::warning::`；
4. 文件内不要写版本号以外的动态信息（如发布日期、下载量），由 GitHub Release 承载。

## 发版流程

```powershell
# 1. 整理发行说明并提交
#    release-notes/v1.2.0.md
git add release-notes/v1.2.0.md && git commit -m "docs: release notes for v1.2.0"

# 2. 同步 tooth.json 的 version 字段（可选，仅 tooth 仓库发布需要）
# 3. 打 tag 并推送 —— 推送即触发 CI 构建 + 发版
git tag v1.2.0
git push origin v1.2.0
```

版本号与产物版本的一致性由 CI 保证：tag → 环境变量 `TS_VERSION` → `manifest.json` 的 `version`。
