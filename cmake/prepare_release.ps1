[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$PreviousVersion,
    [Parameter(Mandatory = $true)][string]$Repository,
    [Parameter(Mandatory = $true)][string]$Commit,
    [string]$ArtifactDirectory = 'artifact',
    [string]$ServerUrl = 'https://github.com'
)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'version.ps1')
$version = (Get-TtpCommBuildVersion $Version).Name
if ($PreviousVersion -and -not (Read-TtpCommVersionTag $PreviousVersion)) { throw 'Invalid previous release version.' }
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or $Commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw 'Missing or invalid repository/commit.'
}
$archiveName = "ttpcomm-$version.zip"
$archive = Join-Path $ArtifactDirectory $archiveName
$sourceName = "ttpcomm-$version-source.zip"
$sourceArchive = Join-Path $ArtifactDirectory $sourceName
$manifest = Join-Path $ArtifactDirectory 'SHA256SUMS.txt'
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath $sourceArchive -Algorithm SHA256).Hash.ToLowerInvariant()
$expectedHashes = (Get-Content -LiteralPath $manifest -Encoding UTF8 -Raw).Trim() -replace "`r", ''
if ($expectedHashes -cne "$hash  $archiveName`n$sourceHash  $sourceName") {
    throw 'TTPCOMM package SHA-256 verification failed.'
}
# Version was fixed before compilation. Never rename an already-built package.
$repoUrl = "$ServerUrl/$Repository"
$logUrl = if ($PreviousVersion) { "$repoUrl/compare/$PreviousVersion...$version" }
          else { "$repoUrl/commits/$version" }
$notes = @"
**更新记录**: $logUrl

关闭播放器后，将 ttpcomm-$version.zip 中的 ttpcomm.dll 放入播放器根目录。
同一 x86 DLL 兼容 XP SP3、Win7 和新系统，接口版本保持 0x00050700。
运行包只包含 ttpcomm.dll 与 SHA256SUMS.txt；许可证保存在仓库和源码包，不嵌入 DLL。
源码包包含本项目与所用的固定 GPL/LGPL 等依赖源码；CoolSB 和兼容工具链仍按固定版本下载。

[实现、差异与验证范围]($repoUrl/blob/$Commit/docs/RECONSTRUCTION_STATUS.md)
[第三方来源与许可证]($repoUrl/blob/$Commit/licenses/README.md)
"@
$notes | Set-Content -LiteralPath (Join-Path $ArtifactDirectory 'release-notes.md') -Encoding UTF8
