# Downloads the vendor SDK files SaturnProject builds against into third_party/ (they are not stored in this
# repository because their licenses are not AGPL-compatible). Sources are the official GitHub repositories:
#   NVIDIA DLSS SDK         https://github.com/NVIDIA/DLSS                     (NVIDIA RTX SDKs license)
#   AMD FidelityFX SDK      https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK   (AMD license, signed binaries)
# Usage: powershell -ExecutionPolicy Bypass -File tools\fetch_sdks.ps1
param(
	[string]$DlssTag = 'v310.9.16',
	[string]$FfxTag = 'v2.4.0'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root 'third_party'
$headers = @{ 'User-Agent' = 'SaturnProject-fetch-sdks' }

function Get-Tree($repo, $tag) {
	(Invoke-RestMethod -Headers $headers "https://api.github.com/repos/$repo/git/trees/$($tag)?recursive=1").tree | Where-Object { $_.type -eq 'blob' }
}
function Get-File($repo, $tag, $path, $dest) {
	New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
	Invoke-WebRequest -Headers $headers -UseBasicParsing "https://raw.githubusercontent.com/$repo/$tag/$path" -OutFile $dest
}

Write-Host "NVIDIA DLSS SDK $DlssTag..."
$repo = 'NVIDIA/DLSS'
$tree = Get-Tree $repo $DlssTag
foreach ($f in $tree | Where-Object { $_.path -like 'include/*.h' }) { Get-File $repo $DlssTag $f.path (Join-Path $tp "dlss/$($f.path)") }
Get-File $repo $DlssTag 'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib' (Join-Path $tp 'dlss/lib/nvsdk_ngx_s.lib')
Get-File $repo $DlssTag 'lib/Windows_x86_64/rel/nvngx_dlssg.dll' (Join-Path $tp 'dlss/lib/nvngx_dlssg.dll')
Get-File $repo $DlssTag 'LICENSE.txt' (Join-Path $tp 'dlss/LICENSE.txt')

Write-Host "AMD FidelityFX SDK $FfxTag..."
$repo = 'GPUOpen-LibrariesAndSDKs/FidelityFX-SDK'
$tree = Get-Tree $repo $FfxTag
foreach ($f in $tree | Where-Object { $_.path -like 'Kits/FidelityFX/api/include/*' -or $_.path -like 'Kits/FidelityFX/framegeneration/include/*' }) {
	Get-File $repo $FfxTag $f.path (Join-Path $tp ("fsr/" + $f.path.Substring('Kits/FidelityFX/'.Length)))
}
foreach ($dll in 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll') {
	Get-File $repo $FfxTag "Kits/FidelityFX/signedbin/$dll" (Join-Path $tp "fsr/bin/$dll")
}
Get-File $repo $FfxTag 'Kits/FidelityFX/docs/license.md' (Join-Path $tp 'fsr/LICENSE.md')

$sig = (Get-AuthenticodeSignature (Join-Path $tp 'dlss/lib/nvngx_dlssg.dll')).SignerCertificate.Subject
Write-Host "nvngx_dlssg.dll signed by: $sig"
$sig = (Get-AuthenticodeSignature (Join-Path $tp 'fsr/bin/amd_fidelityfx_framegeneration_dx12.dll')).SignerCertificate.Subject
Write-Host "amd_fidelityfx_framegeneration_dx12.dll signed by: $sig"
Write-Host 'Done. Configure CMake again: DLSS and FSR 3 frame generation are enabled automatically.'
