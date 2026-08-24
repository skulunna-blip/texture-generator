# 一键发布到 GitHub。
# 前提：先在浏览器 https://github.com/new 建好空仓库：
#   - 仓库名：texture-generator（一字不差）
#   - 可见性：Public（issues 页要公开，审核要查 bug tracker）
#   - 不要勾选自动生成 README / .gitignore / license（本地都有，勾了 push 会冲突）
# 建好后运行：
#   powershell -NoProfile -ExecutionPolicy Bypass -File _git_publish.ps1
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$repo = "https://github.com/skulunna-blip/texture-generator.git"

# 判断 origin 是否已存在（用 get-url，不用 $LASTEXITCODE 判断管道）
$existing = git remote get-url origin 2>$null
if (-not $existing) {
    git remote add origin $repo
    Write-Host "remote origin added: $repo"
} else {
    git remote set-url origin $repo
    Write-Host "remote origin updated: $repo"
}

git branch -M main
git push -u origin main
Write-Host "Pushed. Issues page: https://github.com/skulunna-blip/texture-generator/issues"

# 推送后的校验：应显示 "## main...origin/main"
git status -sb