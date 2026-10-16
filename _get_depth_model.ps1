# 下载 AI 深度模型 + ONNX Runtime 到 app\models\ 与 app\third_party\onnxruntime\。
# 运行：powershell -NoProfile -ExecutionPolicy Bypass -File _get_depth_model.ps1
#
# 这两样东西体积较大（合计约 61 MB），所以没有直接塞进 git 仓库，而是用这个脚本拉。
# 拉完之后重新构建一次，CMake 的 POST_BUILD 会把它们复制到 build\ 目录。
#
# 注意：只下载 Small 权重。上游明确写了 Small 是 Apache-2.0，
# Base/Large/Giant 是 CC-BY-NC-4.0（非商用），不能随本软件分发。
$ErrorActionPreference = "Stop"

$repo = $PSScriptRoot
$modelsDir = Join-Path $repo "app\models"
$ortDir = Join-Path $repo "app\third_party\onnxruntime"
$ortBin = Join-Path $ortDir "bin"
$ortInclude = Join-Path $ortDir "include"
$tmp = Join-Path $repo "dist\_download"

$modelFile = Join-Path $modelsDir "depth_anything_v2_small_fp16.onnx"
$ortVersion = "1.20.1"
$ortZipName = "onnxruntime-win-x64-$ortVersion.zip"
$ortUrl = "https://github.com/microsoft/onnxruntime/releases/download/v$ortVersion/$ortZipName"

# HuggingFace serves the same file from a CDN; the mirror is the fallback.
$modelUrls = @(
    "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model_fp16.onnx",
    "https://hf-mirror.com/onnx-community/depth-anything-v2-small/resolve/main/onnx/model_fp16.onnx"
)

New-Item -ItemType Directory -Force -Path $modelsDir, $ortBin, $ortInclude, $tmp | Out-Null

function Get-FirstWorkingUrl {
    param([string[]]$Urls, [string]$Destination, [long]$MinBytes, [string]$Label)

    if ((Test-Path $Destination) -and ((Get-Item $Destination).Length -ge $MinBytes)) {
        Write-Host "$Label already present."
        return $true
    }
    foreach ($u in $Urls) {
        try {
            Write-Host "Downloading $Label from $u"
            curl.exe -L --fail --connect-timeout 20 -o $Destination $u
            if ((Test-Path $Destination) -and ((Get-Item $Destination).Length -ge $MinBytes)) {
                return $true
            }
        }
        catch {
            Write-Host "  failed, trying next mirror"
        }
    }
    return $false
}

# --- 1. the model ---------------------------------------------------------
if (-not (Get-FirstWorkingUrl -Urls $modelUrls -Destination $modelFile -MinBytes 10MB -Label "depth model")) {
    throw "Could not download the depth model. Grab model_fp16.onnx from https://huggingface.co/onnx-community/depth-anything-v2-small/tree/main/onnx and save it as $modelFile"
}

# --- 2. ONNX Runtime ------------------------------------------------------
if (-not (Test-Path (Join-Path $ortBin "onnxruntime.dll"))) {
    $zip = Join-Path $tmp $ortZipName
    if (-not (Get-FirstWorkingUrl -Urls @($ortUrl) -Destination $zip -MinBytes 10MB -Label "ONNX Runtime $ortVersion")) {
        throw "Could not download ONNX Runtime. Grab onnxruntime-win-x64-$ortVersion.zip from https://github.com/microsoft/onnxruntime/releases and extract it into $ortDir"
    }

    $extract = Join-Path $tmp "ort"
    if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $extract -Force

    $root = Join-Path $extract "onnxruntime-win-x64-$ortVersion"
    Copy-Item (Join-Path $root "lib\onnxruntime.dll") $ortBin -Force
    Copy-Item (Join-Path $root "include\*.h") $ortInclude -Force
    Copy-Item (Join-Path $root "LICENSE") $ortDir -Force
    Write-Host "Extracted ONNX Runtime into $ortDir"
}
else {
    Write-Host "ONNX Runtime already present."
}

# --- 3. licence that has to travel with the model -------------------------
$license = Join-Path $modelsDir "LICENSE-DepthAnythingV2-Apache-2.0.txt"
if (-not (Test-Path $license)) {
    try {
        curl.exe -L --fail --connect-timeout 20 -o $license `
            "https://raw.githubusercontent.com/DepthAnything/Depth-Anything-V2/main/LICENSE"
        Write-Host "Downloaded the Apache-2.0 licence text."
    }
    catch {
        Write-Host "WARNING: could not fetch the licence text; copy LICENSE-DepthAnythingV2-Apache-2.0.txt in by hand before distributing."
    }
}

Write-Host ""
Write-Host "Done. Rebuild so the POST_BUILD copy picks these up:"
Write-Host "  cmake --build `"$repo\app\build`" --target TextureGeneratorQt"
