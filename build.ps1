[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'build'),
    [string]$Generator = 'Visual Studio 18 2026',
    [string[]]$CMakeArguments = @(),
    [switch]$Package,
    [switch]$SourcePackage,
    [switch]$SkipBuild,
    [string]$PackageVersion
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cmake/version.ps1')
$PackageVersion = (Get-TtpCommBuildVersion $PackageVersion).Name
function Invoke-CMake([string[]]$Arguments) {
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) { throw "CMake failed ($LASTEXITCODE)." }
}
if (-not $SkipBuild) {
    Invoke-CMake (@('-S',$PSScriptRoot,'-B',$BuildDirectory,'-G',$Generator,'-A','Win32') + $CMakeArguments +
        @("-DTTPCOMM_BUILD_VERSION=$PackageVersion"))
    Invoke-CMake @('--build',$BuildDirectory,'--config','Release','--target','ttpcomm_core','--parallel','4')
}
$output = Join-Path $BuildDirectory 'Release'
$dll = Join-Path $output 'ttpcomm.dll'
Assert-TtpCommFileVersion $dll $PackageVersion
$hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
$abi = Get-Content -LiteralPath (Join-Path $output 'abi-report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$imports = Get-Content -LiteralPath (Join-Path $output 'legacy-imports.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $abi.complete -or $abi.legacy_export_count -ne 67 -or $abi.extension_export_count -ne 2 -or
    @($abi.exports.PSObject.Properties).Count -ne 69 -or
    $abi.sha256 -cne $hash -or $imports.sha256 -cne $hash -or
    $imports.minimum_subsystem -ne '5.01' -or $imports.architecture -ne 'x86' -or
    $imports.inventories -notcontains '5.1.2600.txt' -or $imports.inventories -notcontains '6.1.7600.txt') {
    throw 'The DLL must pass complete ABI and XP/Win7 import audits before packaging.'
}
if (-not ($Package -or $SourcePackage)) { return }
$stage = Join-Path $BuildDirectory ('package-' + [guid]::NewGuid().ToString('N'))
$binary = Join-Path $stage 'binary'
New-Item -ItemType Directory -Path $binary -Force | Out-Null
Invoke-CMake @('--install',$BuildDirectory,'--config','Release','--component','Runtime','--prefix',$binary)
"$hash  ttpcomm.dll" | Set-Content -LiteralPath (Join-Path $binary 'SHA256SUMS.txt') -Encoding UTF8
$archive = Join-Path $output "ttpcomm-$PackageVersion.zip"
Compress-Archive -LiteralPath (Join-Path $binary 'ttpcomm.dll'),(Join-Path $binary 'SHA256SUMS.txt') -DestinationPath $archive -Force
$archives = @($archive)
if ($SourcePackage) {
    $source = Join-Path $stage 'source/ttpcomm'
    New-Item -ItemType Directory -Path $source -Force | Out-Null
    foreach ($entry in @('CMakeLists.txt','build.ps1','README.md','.gitignore','cmake','src','include','sdk','abi','docs','licenses','.github')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $entry) -Destination $source -Recurse
    }
    $PackageVersion | Set-Content -LiteralPath (Join-Path $source 'BUILD_VERSION') -Encoding ASCII
    $upstreams = Join-Path $source 'third_party_sources'
    New-Item -ItemType Directory -Path $upstreams | Out-Null
    foreach ($line in Get-Content -LiteralPath (Join-Path $BuildDirectory 'dependency-sources.txt') -Encoding UTF8) {
        $parts = $line -split '=',2
        if ($parts.Count -ne 2 -or $parts[0] -notin @('zlib','id3','kissfft','ssrc','mpeg','dream','detours')) { throw 'Invalid dependency source manifest.' }
        Copy-Item -LiteralPath $parts[1] -Destination (Join-Path $upstreams $parts[0]) -Recurse
    }
    # CoolSB and the compatibility toolchain retain their fixed download paths.
    # The private tests, reference binaries and generated build trees are excluded.
    $sourceArchive = Join-Path $output "ttpcomm-$PackageVersion-source.zip"
    Compress-Archive -LiteralPath $source -DestinationPath $sourceArchive -Force
    $archives += $sourceArchive
}
Get-FileHash -LiteralPath $archives -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash.ToLowerInvariant(),[IO.Path]::GetFileName($_.Path) } |
    Set-Content -LiteralPath (Join-Path $output 'PACKAGE-SHA256SUMS.txt') -Encoding UTF8
Write-Output "TTPCOMM packages: $output"
