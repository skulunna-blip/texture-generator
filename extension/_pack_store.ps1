# Pack store/ contents into a flat extension zip for extensions.blender.org.
# The zip must contain blender_manifest.toml at the ROOT (no <id>/ folder prefix) —
# unlike classic addons, which wrap files in a top-level folder.
$ErrorActionPreference = "Stop"
$src = "C:\Users\hasee\texture-generator\extension"
$out = "C:\Users\hasee\texture-generator\dist\tg_blender_link.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path $out) { Remove-Item $out -Force }
$zip = [System.IO.Compression.ZipFile]::Open($out, 'Create')
foreach ($f in @('blender_manifest.toml','__init__.py','README.md','LICENSE')) {
    $entry = $zip.CreateEntry($f)   # flat: no folder prefix
    $es = $entry.Open()
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $src $f))
    $es.Write($bytes, 0, $bytes.Length)
    $es.Close()
}
$zip.Dispose()
Write-Host "Packed: $out"
