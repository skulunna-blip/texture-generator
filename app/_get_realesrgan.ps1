# 下载并解压 Real-ESRGAN (realesrgan-ncnn-vulkan) 到 tools/。
# 运行：powershell -NoProfile -ExecutionPolicy Bypass -File _get_realesrgan.ps1
# 之后应用会从 tools/realesrgan-ncnn-vulkan-20220424-windows/ 自动找到它。
$ErrorActionPreference = "Stop"

$tools = Join-Path $PSScriptRoot "tools"
$zip = Join-Path $tools "realesrgan-ncnn-vulkan-20220424-windows.zip"
$extractDir = Join-Path $tools "realesrgan-ncnn-vulkan-20220424-windows"

$urls = @(
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip",
    "https://ghproxy.net/https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip",
    "https://gh-proxy.com/https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip"
)

New-Item -ItemType Directory -Force -Path $tools | Out-Null

if (Test-Path (Join-Path $extractDir "realesrgan-ncnn-vulkan.exe")) {
    Write-Host "Already present: $extractDir"
    exit 0
}

function Test-Downloaded {
    return (Test-Path $zip) -and ((Get-Item $zip).Length -gt 1MB)
}

$done = $false
foreach ($u in $urls) {
    # curl.exe is the fast path, but it ignores the Windows proxy settings that
    # Invoke-WebRequest picks up, so a machine behind a proxy fails here and
    # succeeds there. Try both before moving on to the next mirror.
    try {
        Write-Host "Downloading $u (curl)"
        if (Test-Path $zip) { Remove-Item $zip -Force }
        curl.exe -L --fail --connect-timeout 15 -o $zip $u
        if (Test-Downloaded) { $done = $true; break }
    }
    catch {
        Write-Host "  curl failed"
    }

    try {
        Write-Host "  retrying with Invoke-WebRequest"
        if (Test-Path $zip) { Remove-Item $zip -Force }
        Invoke-WebRequest -Uri $u -OutFile $zip -UseBasicParsing -TimeoutSec 120
        if (Test-Downloaded) { $done = $true; break }
    }
    catch {
        Write-Host "  failed, trying next mirror"
    }
}
if (-not $done) {
    throw "Could not download Real-ESRGAN. Please grab the zip manually and extract it to: $extractDir"
}

Expand-Archive -Path $zip -DestinationPath $tools -Force
$exe = Join-Path $extractDir "realesrgan-ncnn-vulkan.exe"
if (-not (Test-Path $exe)) {
    throw "Extracted binary not found at $exe"
}
Write-Host "OK: $exe"
