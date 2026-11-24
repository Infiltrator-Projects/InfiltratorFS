# SPDX-License-Identifier: GPL-3.0-or-later
# Bounded smoke test on an already-mounted dedicated native Windows volume.
# Never installs drivers, formats disks, enables Verifier or restarts services.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidatePattern('^[A-Za-z]$')]
    [string] $DriveLetter,
    [Parameter(Mandatory=$true)]
    [switch] $DedicatedTestVolume
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$DedicatedTestVolume) { throw 'Explicit dedicated-volume consent required.' }
$root = $DriveLetter.ToUpperInvariant() + ':\'
if ($root -ieq [System.IO.Path]::GetPathRoot($env:SystemRoot)) { throw 'System volume forbidden.' }
$drive = [System.IO.DriveInfo]::new($root)
if (!$drive.IsReady -or $drive.DriveFormat -ine 'InfiltratorFS') {
    throw 'Native InfiltratorFS volume required, not NTFS/ProjFS.'
}
$driver = Get-CimInstance Win32_SystemDriver -Filter "Name='InfiltratorFS'"
$service = Get-CimInstance Win32_Service -Filter "Name='InfiltratorFSNative'"
if (!$driver -or $driver.State -ne 'Running' -or !$service -or $service.State -ne 'Running') {
    throw 'Native driver and core service must already be running.'
}
$source = & git -C (Join-Path $PSScriptRoot '..') rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify qualification source.' }
Write-Host "Source: $source; Windows: $([Environment]::OSVersion.VersionString)"
Write-Host "Driver: $($driver.PathName); Service: $($service.PathName); Target: $root"
Write-Host 'Record actual installed binary hashes independently.'
$dir = Join-Path $root ('infilfs-native-qualification-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $dir) { throw 'Unexpected scratch-path collision.' }
[void][System.IO.Directory]::CreateDirectory($dir)
$file = Join-Path $dir 'payload.bin'
$renamed = Join-Path $dir 'renamed.bin'
$link = Join-Path $dir 'hardlink.bin'
$bytes = [byte[]]::new(1048576)
$rng = [System.Random]::new(1945)
$rng.NextBytes($bytes)
function Assert-Content([string] $Path, [byte[]] $Expected) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = [BitConverter]::ToString($sha.ComputeHash($Expected)).Replace('-', '')
        if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $hash) {
            throw "SHA-256 mismatch: $Path"
        }
    } finally { $sha.Dispose() }
}
try {
    $stream = [System.IO.FileStream]::new($file, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    Assert-Content $file $bytes
    Write-Host 'PASS: cached write, durable flush, close/reopen and SHA-256.'
    # Paging I/O and cached reads must agree after the mapping closes.
    $mapping = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile(
        $file, [System.IO.FileMode]::Open, $null, 0,
        [System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::ReadWrite)
    try {
        $view = $mapping.CreateViewAccessor()
        try {
            if ($view.ReadByte(4097) -ne $bytes[4097]) { throw 'Mapped read mismatch.' }
            $bytes[4097] = [byte]($bytes[4097] -bxor 255)
            $view.Write([long]4097, [byte]$bytes[4097])
            $view.Flush()
        } finally { $view.Dispose() }
    } finally { $mapping.Dispose() }
    Assert-Content $file $bytes
    Write-Host 'PASS: memory-mapped read/write and cached read coherence.'
    [System.IO.File]::Move($file, $renamed)
    if ((Test-Path -LiteralPath $file) -or !(Test-Path -LiteralPath $renamed)) {
        throw 'Rename did not update namespace.'
    }
    [void](New-Item -ItemType HardLink -Path $link -Target $renamed)
    Assert-Content $link $bytes
    $stream = [System.IO.FileStream]::new($link, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
    try {
        $bytes[0] = [byte]($bytes[0] -bxor 255)
        $stream.WriteByte($bytes[0])
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    Assert-Content $renamed $bytes
    [System.IO.File]::Delete($link)
    Assert-Content $renamed $bytes
    if (@([System.IO.Directory]::EnumerateFiles($dir)).Count -ne 1) {
        throw 'Unexpected directory enumeration.'
    }
    $acl = Get-Acl -LiteralPath $renamed
    if ([string]::IsNullOrWhiteSpace($acl.Sddl)) { throw 'Missing security descriptor.' }
    Write-Host 'PASS: rename, hard-link coherence, unlink, enumeration and ACL query.'
    [System.IO.File]::Delete($renamed)
    [System.IO.Directory]::Delete($dir)
    Write-Host 'PASS: bounded native Windows mounted smoke qualification only.'
} catch {
    Write-Warning "Qualification failed; scratch evidence retained at $dir"
    throw
}
