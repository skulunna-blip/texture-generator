# Build a portable (免安装) distribution of TextureGeneratorQt from the
# already-deployed app\build folder (windeployqt has run, so Qt DLLs are present).
#
# 产物：<repo>\dist\TextureGeneratorQt-portable.zip
#
# Paths are derived from the script location, so the repo can be moved without
# silently packaging a stale build somewhere else.
$ErrorActionPreference = "Stop"

$repo  = $PSScriptRoot
$build = Join-Path $repo "app\build"
$distRoot = Join-Path $repo "dist"
$stage = Join-Path $distRoot "TextureGeneratorQt-portable"
$out   = Join-Path $distRoot "TextureGeneratorQt-portable.zip"

if (-not (Test-Path (Join-Path $build "TextureGeneratorQt.exe"))) {
    throw "No build found at $build - build the app first."
}

# The AI depth feature is part of the product now, so a package missing either
# half of it is a broken package rather than a lean one. Fail loudly instead of
# shipping a build where the backend silently greys itself out.
$ortDll = Join-Path $build "onnxruntime.dll"
$model  = Join-Path $build "models\depth_anything_v2_small_fp16.onnx"
foreach ($required in @($ortDll, $model)) {
    if (-not (Test-Path $required)) {
        throw "Missing $required - rebuild the app so the POST_BUILD copy runs."
    }
}

# 1. Fresh staging folder
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $out)   { Remove-Item $out -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# 2. Copy runtime files (skip build-system dirs and test artifacts)
Get-ChildItem $build | ForEach-Object {
    $skip = @('CMakeCache.txt','CMakeFiles','TextureGeneratorQt_autogen','build.ninja',
              'cmake_install.cmake','orient.png','tg_ball_shot.png','.ninja_deps','.qt','.ninja_log',
              'models')
    if ($skip -contains $_.Name) { return }
    if ($_.Name -like 'rot_*.png') { return }
    if ($_.Name -like '*.log') { return }
    if ($_.Name -like '*_autogen') { return }
    # test executables: keep only the main app exe
    if ($_.Name -like '*.exe' -and $_.Name -ne 'TextureGeneratorQt.exe') { return }
    Copy-Item $_.FullName $stage -Recurse -Force
}

# 3. models\ - the depth model plus its licence, which Apache-2.0 requires to
#    travel with the binary.
$modelsStage = Join-Path $stage "models"
New-Item -ItemType Directory -Force -Path $modelsStage | Out-Null
Copy-Item $model $modelsStage -Force
foreach ($doc in @('README.txt', 'LICENSE-DepthAnythingV2-Apache-2.0.txt')) {
    $src = Join-Path $repo "app\models\$doc"
    if (Test-Path $src) { Copy-Item $src $modelsStage -Force }
}

# 4. ONNX Runtime's MIT notice (the DLL itself is already in the stage root).
$ortLicense = Join-Path $repo "app\third_party\onnxruntime\LICENSE"
if (Test-Path $ortLicense) {
    Copy-Item $ortLicense (Join-Path $stage "LICENSE-onnxruntime-MIT.txt") -Force
}

# 5. Real-ESRGAN upscaler. It ships inside the package so the upscale button
#    works offline out of the box; asking users to fetch a 45 MB zip and unpack
#    it by hand was the single most common thing to go wrong.
$tools = Join-Path $stage "tools"
New-Item -ItemType Directory -Force -Path $tools | Out-Null
$resrSrc = Join-Path $repo "app\tools\realesrgan-ncnn-vulkan"
if (Test-Path (Join-Path $resrSrc "realesrgan-ncnn-vulkan.exe")) {
    Copy-Item $resrSrc (Join-Path $tools "realesrgan-ncnn-vulkan") -Recurse -Force
}
else {
    # A package without the upscaler is still usable, but say so in the archive
    # instead of leaving an empty folder that looks like a packaging bug.
    @"
Real-ESRGAN 超分放大（可选）放置目录。

本包没有内置 Real-ESRGAN。要启用"无损放大"，把 realesrgan-ncnn-vulkan 解压到本目录，使其包含：
    tools\realesrgan-ncnn-vulkan\realesrgan-ncnn-vulkan.exe
    tools\realesrgan-ncnn-vulkan\models\...        (realesrgan-x4plus / x4plus-anime)

下载：https://github.com/xinntao/Real-ESRGAN/releases
   (v0.2.5.0 的 realesrgan-ncnn-vulkan-20220424-windows.zip 即可)
或者在源码目录运行 app\_get_realesrgan.ps1 后重新打包。

This package does not bundle Real-ESRGAN. Unpack it into this folder to enable
the upscale button; see above for the exact layout.
"@ | Set-Content -Path (Join-Path $tools "README.txt") -Encoding UTF8
}
$resrLicense = Join-Path $repo "app\tools\LICENSE-RealESRGAN-BSD-3-Clause.txt"
if (Test-Path $resrLicense) {
    Copy-Item $resrLicense (Join-Path $stage "LICENSE-RealESRGAN-BSD-3-Clause.txt") -Force
}

# 6. Zip (flat folder, no nested wrapper)
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $out)

Write-Host "Portable package ready: $out"
Write-Host "  size: $([math]::Round((Get-Item $out).Length/1MB, 1)) MB"
