$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

# BUILD 17-r3 preparation repairs only two BUILD 17 transform defects discovered by CI:
# 1) PowerShell output-stream pollution in Count-Occurrences.
# 2) One extra replacement field in the compute-forensics fmt::format signature.
# Runtime policy is unchanged: this remains diagnostics-only and SafeGPU compute stays fail-closed.
$Path = Join-Path $env:GITHUB_WORKSPACE 'experiments/build17/apply-build17.ps1'
$Text = [System.IO.File]::ReadAllText($Path).Replace("`r`n", "`n")

function Replace-ExactlyOnce([string]$Source, [string]$Old, [string]$New, [string]$Description) {
    $Index = $Source.IndexOf($Old)
    if ($Index -lt 0 -or $Source.IndexOf($Old, $Index + $Old.Length) -ge 0) {
        throw "Build 17-r3 preparation failed: $Description was not found exactly once"
    }
    return $Source.Substring(0, $Index) + $New + $Source.Substring($Index + $Old.Length)
}

$OldCounter = @'
        ++$Count
'@
$NewCounter = @'
        $Count += 1
'@
$Text = Replace-ExactlyOnce $Text $OldCounter $NewCounter 'Count-Occurrences increment'

# The BUILD 17 recorder passes exactly 24 arguments. The original format string accidentally
# contained 25 replacement fields, causing fmt's compile-time checker to report "arg id not found".
$OldFormat = @'
"{},{},{},0x{:016x},0x{:016x},0x{:016x},0x{:08x},{},{},{},{},{},{},{},{},{},{},{},{},{},{},0x{:08x},0x{:016x},{},0x{:016x}",
'@.Trim()
$NewFormat = @'
"{},{},{},0x{:016x},0x{:016x},0x{:016x},0x{:08x},{},{},{},{},{},{},{},{},{},{},{},{},{},0x{:08x},0x{:016x},{},0x{:016x}",
'@.Trim()
$Text = Replace-ExactlyOnce $Text $OldFormat $NewFormat 'compute-forensics fmt signature'

[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

$Verify = [System.IO.File]::ReadAllText($Path).Replace("`r`n", "`n")
if ($Verify.Contains('++$Count') -or -not $Verify.Contains('$Count += 1')) {
    throw 'Build 17-r3 preparation failed verification: scalar counter fix was not applied'
}
if ($Verify.Contains($OldFormat) -or -not $Verify.Contains($NewFormat)) {
    throw 'Build 17-r3 preparation failed verification: fmt signature arity fix was not applied'
}

Write-Host 'Build 17-r3 preparation verified: scalar counter and 24-field compute-forensics format signature staged.'
