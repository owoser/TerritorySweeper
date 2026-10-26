# 修复 xmake 3.1.1 在拉取 LeviLamina 生态仓库时的 semver 解析问题
#
# 背景：levimc-repo / yyz-repo 中部分包（bdslibrary、bedrockdata 旧版本等）使用四段式版本号
#       （如 1.21.3.01），xmake 3.1.1 的 semver 解析无法处理，会报
#       "unable to parse semver" 并导致所有 add_requires 失败。
#
# 本脚本做三件事（幂等，可重复运行）：
#   1. 删除项目中未使用的 bdslibrary 包定义
#   2. 移除老版本 levilamina 依赖文件中的 bdslibrary 依赖
#   3. 归一化全局 quick_search 缓存中的四段式版本号
#
# 注意：执行 xmake repo -u 之后需要重新运行本脚本。

$ErrorActionPreference = 'SilentlyContinue'

$projectRoot = Split-Path -Parent $PSScriptRoot
$repoDir     = Join-Path $projectRoot '.xmake\windows\x64\repositories\levimc-repo\packages'
$cacheFile   = Join-Path $env:LOCALAPPDATA '.xmake\cache\quick_search'

Write-Host "[1/3] 清理 bdslibrary 包定义 ..."
$bds = Join-Path $repoDir 'b\bdslibrary'
if (Test-Path $bds) {
    Remove-Item $bds -Recurse -Force
    Write-Host "      removed $bds"
} else {
    Write-Host "      (已清理，跳过)"
}

Write-Host "[2/3] 修正老版本 levilamina 依赖 ..."
foreach ($f in @('0_13_4.lua', '0_13_5.lua')) {
    $p = Join-Path $repoDir "l\levilamina\versions\$f"
    if (Test-Path $p) {
        (Get-Content $p) | Where-Object { $_ -notmatch 'bdslibrary' } | Set-Content $p
        Write-Host "      patched $f"
    }
}

Write-Host "[3/3] 归一化 quick_search 缓存版本号 ..."
if (Test-Path $cacheFile) {
    $raw = Get-Content $cacheFile -Raw
    $patched = [regex]::Replace($raw, '"v?(\d+)\.(\d+)\.(\d+)\.(\d+)([^"\r\n]*)"', '"$1.$2.$3-$4$5"')
    Set-Content -Path $cacheFile -Value $patched -NoNewline
    Write-Host "      normalized"
} else {
    Write-Host "      (缓存不存在，下次配置时会重新生成；若仍报错请再运行一次本脚本)"
}

Write-Host ""
Write-Host "完成。现在可以执行: xmake f -y -p windows -a x64 -m release" -ForegroundColor Green
