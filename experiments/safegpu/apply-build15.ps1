$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Normalize([string]$Text) {
    return $Text.Replace("`r`n", "`n")
}

# BUILD 15 is a one-axis correction on top of BUILD 14.
# BUILD 12 changed SafeGpuGraphicsPipelineInfo.has_stencil from "stencil test enabled"
# semantics to "an actual Vulkan stencil attachment exists" semantics. BUILD 11's
# empirically proven Driveclub depth exception still required !info.has_stencil, which
# unintentionally filtered almost all of the 110 hashes already proven native-safe.
# BUILD 15 changes only that exact Driveclub proven-hash exception: a proven hash may
# use its native depth pipeline even when the valid depth attachment includes stencil.
# Unknown depth/stencil pipelines remain governed by the BUILD 12 strict rejection.

# Give runtime logs an unambiguous policy identity.
$Path = 'src/video_core/safe_gpu/safe_gpu.h'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$OldPolicy = 'return "milestone-6-driveclub-native-all-admitted-depthless-v1";'
$NewPolicy = 'return "milestone-7-driveclub-restore-proven-depth-v1";'
$PolicyIndex = $Text.IndexOf($OldPolicy)
if ($PolicyIndex -lt 0 -or $Text.IndexOf($OldPolicy, $PolicyIndex + 1) -ge 0) {
    throw 'Build 15 transform failed: BUILD 14 policy identity was not found exactly once'
}
$Text = $Text.Substring(0, $PolicyIndex) + $NewPolicy +
        $Text.Substring($PolicyIndex + $OldPolicy.Length)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Structurally locate SafeGpuGate::ShouldAllowGraphicsPipeline so BUILD 15 only
# changes the old BUILD 11 Driveclub proven-native exception inside this function.
$Path = 'src/video_core/safe_gpu/safe_gpu.cpp'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$Signature = 'bool SafeGpuGate::ShouldAllowGraphicsPipeline('
$Start = $Text.IndexOf($Signature)
if ($Start -lt 0 -or $Text.IndexOf($Signature, $Start + 1) -ge 0) {
    throw 'Build 15 transform failed: ShouldAllowGraphicsPipeline signature was not found exactly once'
}

$OpenBrace = $Text.IndexOf('{', $Start)
if ($OpenBrace -lt 0) {
    throw 'Build 15 transform failed: ShouldAllowGraphicsPipeline opening brace was not found'
}

$Depth = 0
$End = -1
for ($i = $OpenBrace; $i -lt $Text.Length; ++$i) {
    if ($Text[$i] -eq '{') {
        ++$Depth
    } elseif ($Text[$i] -eq '}') {
        --$Depth
        if ($Depth -eq 0) {
            $End = $i
            break
        }
    }
}
if ($End -lt 0) {
    throw 'Build 15 transform failed: ShouldAllowGraphicsPipeline closing brace was not found'
}

$Function = $Text.Substring($Start, $End - $Start + 1)
$DriveclubPredicate = 'IsDriveclubNativeSafeGraphicsPipelineHash(info.pipeline_hash)'
$PredicateIndex = $Function.IndexOf($DriveclubPredicate)
if ($PredicateIndex -lt 0 -or $Function.IndexOf($DriveclubPredicate, $PredicateIndex + 1) -ge 0) {
    throw 'Build 15 transform failed: Driveclub proven-native predicate was not found exactly once'
}

$OldReturn = 'return info.has_depth && !info.has_stencil;'
$ReturnIndex = $Function.IndexOf($OldReturn, $PredicateIndex)
if ($ReturnIndex -lt 0 -or $Function.IndexOf($OldReturn, $ReturnIndex + 1) -ge 0) {
    throw 'Build 15 transform failed: old Driveclub depth-without-stencil return was not found exactly once after the predicate'
}

$NewReturn = @'
// BUILD 15: these exact Driveclub hashes are an empirical native-safe set from the
        // earlier Driveclub experiments. BUILD 12 redefined has_stencil to mean attachment
        // presence, so requiring !has_stencil here accidentally removed the D32S8 members.
        // Restore the proven depth set without admitting any unknown depth pipeline.
        return info.has_depth;
'@
$Function = $Function.Substring(0, $ReturnIndex) + $NewReturn +
            $Function.Substring($ReturnIndex + $OldReturn.Length)
$Text = $Text.Substring(0, $Start) + $Function + $Text.Substring($End + 1)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Fail closed: verify that the new marker is unique and that the old Driveclub
# exception no longer exists inside ShouldAllowGraphicsPipeline.
$Verify = Normalize([System.IO.File]::ReadAllText($Path))
$VerifyStart = $Verify.IndexOf($Signature)
$VerifyOpen = $Verify.IndexOf('{', $VerifyStart)
$VerifyDepth = 0
$VerifyEnd = -1
for ($i = $VerifyOpen; $i -lt $Verify.Length; ++$i) {
    if ($Verify[$i] -eq '{') {
        ++$VerifyDepth
    } elseif ($Verify[$i] -eq '}') {
        --$VerifyDepth
        if ($VerifyDepth -eq 0) {
            $VerifyEnd = $i
            break
        }
    }
}
if ($VerifyEnd -lt 0) {
    throw 'Build 15 transform failed verification: transformed function boundary is invalid'
}
$VerifyFunction = $Verify.Substring($VerifyStart, $VerifyEnd - $VerifyStart + 1)
$Marker = 'BUILD 15: these exact Driveclub hashes are an empirical native-safe set'
$MarkerIndex = $VerifyFunction.IndexOf($Marker)
if ($MarkerIndex -lt 0 -or $VerifyFunction.IndexOf($Marker, $MarkerIndex + 1) -ge 0) {
    throw 'Build 15 transform failed verification: BUILD 15 marker is not unique'
}
if ($VerifyFunction.Contains($OldReturn)) {
    throw 'Build 15 transform failed verification: old Driveclub stencil exclusion still exists'
}

Write-Host 'Build 15 transform verified: Driveclub proven-native depth hashes restored including depth/stencil attachments; unknown depth pipelines remain blocked.'
