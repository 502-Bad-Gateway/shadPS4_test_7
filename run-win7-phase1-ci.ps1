param(
    [string]$OutputDirectory = "$PSScriptRoot\artifact"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ExpectedPayloadSha256 = '1316be1085285763f3c1de985b34201d17372d06f72ded29ebd1a1c0ddad1e2a'
$OldCorruptPayloadSha256 = '35c61fd9e42e289be630edf2b58814f9d407dab5aea5c59a41042edc1a75e544'
$ChunkDir = Join-Path $PSScriptRoot 'ci-payload'
$PayloadZip = Join-Path $PSScriptRoot 'phase1-ci-payload.zip'
$CoreScript = Join-Path $PSScriptRoot 'build-win7-phase1.ps1'
$PatchedCoreScript = Join-Path $PSScriptRoot 'build-win7-phase1-ci-runtime.ps1'

Write-Host '=== Reconstructing audited Phase-1 CI payload ==='
$Chunks = @(Get-ChildItem -Path $ChunkDir -File -Filter 'part*.txt' | Sort-Object Name)
if ($Chunks.Count -ne 9) {
    throw "Expected exactly 9 payload chunks, found $($Chunks.Count)"
}

$Base64 = ($Chunks | ForEach-Object { (Get-Content -Raw $_.FullName).Trim() }) -join ''
try {
    $Bytes = [Convert]::FromBase64String($Base64)
} catch {
    throw "Payload base64 decode failed: $($_.Exception.Message)"
}
[IO.File]::WriteAllBytes($PayloadZip, $Bytes)

$ActualHash = (Get-FileHash -Algorithm SHA256 $PayloadZip).Hash.ToLowerInvariant()
Write-Host "Payload SHA256: $ActualHash"
if ($ActualHash -ne $ExpectedPayloadSha256) {
    throw "Reconstructed payload hash mismatch; expected $ExpectedPayloadSha256"
}

$CoreText = Get-Content -Raw $CoreScript
if (!$CoreText.Contains($OldCorruptPayloadSha256)) {
    throw 'Core controller no longer contains the expected stale payload hash; refusing an uncontrolled edit.'
}
$CoreText = $CoreText.Replace($OldCorruptPayloadSha256, $ExpectedPayloadSha256)
[IO.File]::WriteAllText($PatchedCoreScript, $CoreText, [Text.UTF8Encoding]::new($false))

Write-Host 'Payload verified. Starting the actual Windows 7 target build controller.'
& $PatchedCoreScript -OutputDirectory $OutputDirectory
