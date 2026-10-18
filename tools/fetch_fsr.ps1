# Downloads AMD's FSR frame generation DLLs (MIT-licensed, see third_party/fidelityfx/license.md) from the FSR SDK
# release this add-on is built against, into third_party/fidelityfx/bin, and checks their SHA-256 hashes.
# Used by package_release.bat and the GitHub build; skips files that are already there with the right hash.
$ErrorActionPreference = 'Stop'
$sdkTag = 'v2.3.0'
$base = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$sdkTag/Kits/FidelityFX/signedbin"
$files = [ordered]@{
  'amd_fidelityfx_loader_dx12.dll'          = 'E2D85AA05A9BD9ED8B38935FDF5199372CCA6F74C12015143BB6F945EE1608AA'
  'amd_fidelityfx_framegeneration_dx12.dll' = '02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18'
}
$dir = Join-Path $PSScriptRoot '..\third_party\fidelityfx\bin'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
foreach ($name in $files.Keys) {
  $path = Join-Path $dir $name
  if ((Test-Path $path) -and ((Get-FileHash -Algorithm SHA256 $path).Hash -eq $files[$name])) { continue }
  Write-Host "Downloading $name (FSR SDK $sdkTag)"
  Invoke-WebRequest -UseBasicParsing -Uri "$base/$name" -OutFile $path
  $hash = (Get-FileHash -Algorithm SHA256 $path).Hash
  if ($hash -ne $files[$name]) {
    Remove-Item $path
    throw "$name has SHA-256 $hash, expected $($files[$name])"
  }
}
Write-Host "AMD FSR DLLs ready in $dir"
