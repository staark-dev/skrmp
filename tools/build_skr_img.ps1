# build_skr_img.ps1 — Build GTA SA IMG v2 archive for OP215 custom vehicles
# Usage: .\build_skr_img.ps1 -OutputPath <path> -Files <dff1>,<txd1>,<dff2>,<txd2>,...
#
# GTA SA IMG v2 format:
#   Header:  "VER2" (4 bytes) + uint32 numEntries
#   Directory: numEntries × 32 bytes (offset_sectors:u32, streaming_sectors:u16, size_sectors:u16, name:char[24])
#   Data: files padded to 2048-byte sector boundaries
param(
    [Parameter(Mandatory)][string]$OutputPath,
    [Parameter(Mandatory)][string[]]$Files
)
$ErrorActionPreference = 'Stop'
$SectorSize = 2048

foreach ($f in $Files) {
    if (-not (Test-Path $f)) { throw "File not found: $f" }
}

$entries = @()
foreach ($f in $Files) {
    $item = Get-Item $f
    $name = $item.Name
    if ($name.Length -gt 23) { throw "Filename too long for IMG (max 23 chars): $name" }
    $size = $item.Length
    $sectors = [Math]::Ceiling($size / $SectorSize)
    $entries += [PSCustomObject]@{ Path = $item.FullName; Name = $name; Size = $size; Sectors = $sectors }
}

$numEntries = $entries.Count
$headerSize = 8
$dirSize = $numEntries * 32
$dataStartBytes = $headerSize + $dirSize
$dataStartSectors = [Math]::Ceiling($dataStartBytes / $SectorSize)
$dataStartBytesPadded = $dataStartSectors * $SectorSize

$fs = [System.IO.File]::Create($OutputPath)
try {
    $bw = New-Object System.IO.BinaryWriter($fs)

    # Header
    $bw.Write([byte[]]([System.Text.Encoding]::ASCII.GetBytes("VER2")))
    $bw.Write([uint32]$numEntries)

    # Directory entries
    $currentSector = $dataStartSectors
    foreach ($entry in $entries) {
        $bw.Write([uint32]$currentSector)
        $bw.Write([uint16]$entry.Sectors)
        $bw.Write([uint16]$entry.Sectors)
        $nameBytes = New-Object byte[] 24
        $srcBytes = [System.Text.Encoding]::ASCII.GetBytes($entry.Name)
        [Array]::Copy($srcBytes, $nameBytes, [Math]::Min($srcBytes.Length, 24))
        $bw.Write($nameBytes)
        $currentSector += $entry.Sectors
    }

    # Pad to data start
    $remaining = $dataStartBytesPadded - $fs.Position
    if ($remaining -gt 0) { $bw.Write((New-Object byte[] $remaining)) }

    # File data (each padded to sector boundary)
    foreach ($entry in $entries) {
        $data = [System.IO.File]::ReadAllBytes($entry.Path)
        $bw.Write($data)
        $padded = $entry.Sectors * $SectorSize
        $pad = $padded - $data.Length
        if ($pad -gt 0) { $bw.Write((New-Object byte[] $pad)) }
    }

    $bw.Flush()
    Write-Output "Built IMG: $OutputPath ($($fs.Length) bytes, $numEntries entries)"
    foreach ($entry in $entries) {
        Write-Output "  $($entry.Name): $($entry.Size) bytes ($($entry.Sectors) sectors)"
    }
} finally {
    $fs.Close()
}
