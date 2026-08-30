$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

# BUILD 17-r2 preparation: fix PowerShell output-stream pollution in BUILD 17's
# occurrence counter before the diagnostics-only source transform is executed.
# Prefix increment emits a value in PowerShell, so the helper returned a collection
# instead of one integer and falsely failed the two-dispatch-site verification.
$Path = Join-Path $env:GITHUB_WORKSPACE 'experiments/build17/apply-build17.ps1'
$Text = [System.IO.File]::ReadAllText($Path).Replace("`r`n", "`n")
$Old = @'
        ++$Count
'@
$New = @'
        $Count += 1
'@
$Index = $Text.IndexOf($Old)
if ($Index -lt 0 -or $Text.IndexOf($Old, $Index + 1) -ge 0) {
    throw 'Build 17-r2 preparation failed: Count-Occurrences increment was not found exactly once'
}
$Text = $Text.Substring(0, $Index) + $New + $Text.Substring($Index + $Old.Length)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

$Verify = [System.IO.File]::ReadAllText($Path).Replace("`r`n", "`n")
if ($Verify.Contains('++$Count') -or -not $Verify.Contains('$Count += 1')) {
    throw 'Build 17-r2 preparation failed verification: scalar counter fix was not applied'
}

Write-Host 'Build 17-r2 preparation verified: Count-Occurrences now returns one scalar integer.'
