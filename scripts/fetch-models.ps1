param(
  [string]$Root = "$env:USERPROFILE\asngn",
  [Alias("install-tools")][switch]$InstallTools,
  [switch]$Repair
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dir = Join-Path $Root "models"
$sources = Join-Path $dir ".sources"
New-Item -ItemType Directory -Force $dir, $sources | Out-Null

function Test-Verified([string]$Path, [long]$Size, [string]$Sha) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
  if ((Get-Item -LiteralPath $Path).Length -ne $Size) { return $false }
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Sha
}

$rows = Get-Content -LiteralPath (Join-Path $here "models.manifest.tsv") |
  Where-Object { $_ -and -not $_.StartsWith("#") }
foreach ($row in $rows) {
  $name, $sizeText, $sha, $url, $post = $row -split "`t"
  $size = [long]$sizeText
  $dest = Join-Path $dir $name
  if ($post -eq "-" -and (Test-Verified $dest $size $sha)) {
    Write-Host "verified: $dest"
    continue
  }
  if ($post -eq "-" -and (Test-Path -LiteralPath $dest) -and -not $Repair) {
    throw "verification failed: $dest (use -Repair to replace)"
  }
  if ($post -eq "-") {
    $destPart = "$dest.part"
    if (Test-Path -LiteralPath $destPart) {
      Remove-Item -LiteralPath $destPart -Force
    }
    Write-Host "downloading verified model: $name"
    curl.exe -L --fail --retry 3 -o $destPart $url
    if ($LASTEXITCODE -ne 0 -or -not (Test-Verified $destPart $size $sha)) {
      throw "download verification failed: $name"
    }
    Move-Item -LiteralPath $destPart -Destination $dest -Force
    Write-Host "installed: $dest"
    continue
  }

  $source = Join-Path $sources $name
  if (-not (Test-Verified $source $size $sha)) {
    if ((Test-Path -LiteralPath $source) -and -not $Repair) {
      throw "verification failed: $source (use -Repair to replace)"
    }
    $part = "$source.part"
    if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
    Write-Host "downloading verified source: $name"
    curl.exe -L --fail --retry 3 -o $part $url
    if ($LASTEXITCODE -ne 0 -or -not (Test-Verified $part $size $sha)) {
      throw "download verification failed: $name"
    }
    Move-Item -LiteralPath $part -Destination $source -Force
  }

  $destPart = "$dest.part"
  Copy-Item -LiteralPath $source -Destination $destPart -Force
  $key, $value = $post -split "=", 2
  python (Join-Path $here "gguf_add_kv.py") $destPart $key $value
  if ($LASTEXITCODE -ne 0) { throw "GGUF postprocess failed: $name" }
  Move-Item -LiteralPath $destPart -Destination $dest -Force
  Write-Host "installed: $dest"
}

if ($InstallTools) {
  $pkg = Join-Path (Split-Path -Parent $here) "build\packages"
  if (-not (Test-Path -LiteralPath $pkg -PathType Container)) {
    throw "tool packages not found: $pkg"
  }
  $tools = Join-Path $Root "tools"
  New-Item -ItemType Directory -Force $tools | Out-Null
  Get-ChildItem -LiteralPath $pkg -Directory | ForEach-Object {
    $dst = Join-Path $tools $_.Name
    if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
    Copy-Item -LiteralPath $_.FullName -Destination $dst -Recurse
    Write-Host "tool package: $($_.Name)"
  }
}
