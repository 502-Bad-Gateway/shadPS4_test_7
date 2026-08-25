param(
    [string]$OutputDirectory = "$PSScriptRoot\artifact"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Write-Host 'Phase-2 CI controller revision: 1'

$ExpectedPayloadSha256 = '1316be1085285763f3c1de985b34201d17372d06f72ded29ebd1a1c0ddad1e2a'
$ExpectedPhase2PayloadSha256 = '40ff57a75e20faa30fd41d872663575c40044c28a7b67f47ee01b46430e2a6ea'

$ChunkDir = Join-Path $PSScriptRoot 'ci-payload'
$PayloadZip = Join-Path $PSScriptRoot 'phase1-ci-payload.zip'
$Phase2ChunkDir = Join-Path $PSScriptRoot 'phase2-v17-payload'
$Phase2Zip = Join-Path $PSScriptRoot 'phase2-v17-payload.zip'
$Phase2Dir = Join-Path $PSScriptRoot 'phase2-patches'

Write-Host '=== Reconstructing audited support payload (launcher + FDK patch) ==='
$Chunks = @(Get-ChildItem -Path $ChunkDir -File -Filter 'part*.txt' | Sort-Object Name)
if ($Chunks.Count -ne 9) {
    throw "Expected exactly 9 support-payload chunks, found $($Chunks.Count)"
}

$Base64 = ($Chunks | ForEach-Object { (Get-Content -Raw $_.FullName).Trim() }) -join ''
try {
    $Bytes = [Convert]::FromBase64String($Base64)
} catch {
    throw "Support payload base64 decode failed: $($_.Exception.Message)"
}
[IO.File]::WriteAllBytes($PayloadZip, $Bytes)

$ActualHash = (Get-FileHash -Algorithm SHA256 $PayloadZip).Hash.ToLowerInvariant()
Write-Host "Support payload SHA256: $ActualHash"
if ($ActualHash -ne $ExpectedPayloadSha256) {
    throw "Reconstructed support payload hash mismatch; expected $ExpectedPayloadSha256"
}

Write-Host '=== Reconstructing audited proven-V17 Phase-2 payload ==='
$Phase2Chunks = @(Get-ChildItem -Path $Phase2ChunkDir -File -Filter 'part*.txt' | Sort-Object Name)
if ($Phase2Chunks.Count -ne 11) {
    throw "Expected exactly 11 Phase-2 payload chunks, found $($Phase2Chunks.Count)"
}

$Phase2Base64 = ($Phase2Chunks | ForEach-Object { (Get-Content -Raw $_.FullName).Trim() }) -join ''
try {
    $Phase2Bytes = [Convert]::FromBase64String($Phase2Base64)
} catch {
    throw "Phase-2 payload base64 decode failed: $($_.Exception.Message)"
}
[IO.File]::WriteAllBytes($Phase2Zip, $Phase2Bytes)

$ActualPhase2Hash = (Get-FileHash -Algorithm SHA256 $Phase2Zip).Hash.ToLowerInvariant()
Write-Host "Phase-2 payload SHA256: $ActualPhase2Hash"
if ($ActualPhase2Hash -ne $ExpectedPhase2PayloadSha256) {
    throw "Phase-2 payload hash mismatch; expected $ExpectedPhase2PayloadSha256"
}

Remove-Item -Recurse -Force $Phase2Dir -ErrorAction SilentlyContinue
Expand-Archive -Path $Phase2Zip -DestinationPath $Phase2Dir

$CoreScript = Join-Path $Phase2Dir 'build-win7-phase2-v17.ps1'
if (!(Test-Path $CoreScript)) {
    throw "Phase-2 build controller not found: $CoreScript"
}

Write-Host 'Payloads verified. Starting Phase-2 proven-V17 Windows 7 build.'
& $CoreScript -OutputDirectory $OutputDirectory
