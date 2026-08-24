# Build a portable (免安装) distribution of TextureGeneratorQt from the
# already-deployed build/ folder (windeployqt has run, so Qt DLLs are present).
# 产物：C:\Users\hasee\texture-generator\dist\TextureGeneratorQt-portable.zip
$ErrorActionPreference = "Stop"

$build  = "C:\Users\hasee\Documents\Codex\2026-08-20\blender-claude\outputs\texture-generator-qt\build"
$distRoot = "C:\Users\hasee\texture-generator\dist"
$stage  = Join-Path $distRoot "TextureGeneratorQt"
$out    = Join-Path $distRoot "TextureGeneratorQt-portable.zip"

# 1. Fresh staging folder
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $out)   { Remove-Item $out -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# 2. Copy runtime files (skip build-system dirs and test artifacts)
Get-ChildItem $build | ForEach-Object {
    $skip = @('CMakeCache.txt','CMakeFiles','TextureGeneratorQt_autogen','build.ninja',
              'cmake_install.cmake','orient.png','tg_ball_shot.png','.ninja_deps','.qt','.ninja_log')
    if ($skip -contains $_.Name) { return }
    if ($_.Name -like 'rot_*.png') { return }
    if ($_.Name -like '*.log') { return }
    if ($_.Name -like '*_autogen') { return }
    # test executables: keep only the main app exe
    if ($_.Name -like '*.exe' -and $_.Name -ne 'TextureGeneratorQt.exe') { return }
    Copy-Item $_.FullName $stage -Recurse -Force
}

# 3. tools/ placeholder for the optional Real-ESRGAN upscaler
$tools = Join-Path $stage "tools"
New-Item -ItemType Directory -Force -Path $tools | Out-Null
@"
Real-ESRGAN 超分放大（可选）放置目录。

把 realesrgan-ncnn-vulkan 解压到本目录，使其包含：
    tools\realesrgan-ncnn-vulkan\realesrgan-ncnn-vulkan.exe
    tools\realesrgan-ncnn-vulkan\models\...        (realesrgan-x4plus / x4plus-anime)

不放也能正常用软件，只是"无损放大"按钮会提示未找到 Real-ESRGAN。
下载：https://github.com/xinntao/Real-ESRGAN/releases
   (v0.2.5.0 的 realesrgan-ncnn-vulkan-20220424-windows.zip 即可)

Place the Real-ESRGAN upscaler here (optional). Without it the app works, but
the upscale button reports the binary is missing. See above for the exact layout.
"@ | Set-Content -Path (Join-Path $tools "README.txt") -Encoding UTF8

# 4. Zip (flat folder, no nested wrapper)
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $out)

Write-Host "Portable package ready: $out"
Write-Host "  size: $([math]::Round((Get-Item $out).Length/1MB, 1)) MB"
