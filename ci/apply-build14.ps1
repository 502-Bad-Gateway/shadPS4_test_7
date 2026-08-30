$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Normalize([string]$Text) {
    return $Text.Replace("`r`n", "`n")
}

# BUILD 14 is deliberately a one-axis continuation of BUILD 13.
# It admits no new graphics pipelines and does not relax BUILD 12's strict
# actual depth/stencil attachment gate. For Driveclub only, every graphics
# pipeline that has already passed the existing SafeGPU admission checks now
# keeps its original guest fragment shader and fragment resources instead of
# using the flat replacement shader/resource-suppressed path.

# Runtime policy identity.
$Path = 'src/video_core/safe_gpu/safe_gpu.h'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$OldPolicy = 'return "milestone-5-driveclub-native-depthless-seed-v1";'
$NewPolicy = 'return "milestone-6-driveclub-native-all-admitted-depthless-v1";'
$PolicyIndex = $Text.IndexOf($OldPolicy)
if ($PolicyIndex -lt 0 -or $Text.IndexOf($OldPolicy, $PolicyIndex + 1) -ge 0) {
    throw 'Build 14 transform failed: BUILD 13 policy identity was not found exactly once'
}
$Text = $Text.Substring(0, $PolicyIndex) + $NewPolicy +
        $Text.Substring($PolicyIndex + $OldPolicy.Length)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Replace ShouldUseFlatFragment as a whole function. This deliberately avoids
# depending on whitespace or the exact placement of BUILD 13's final fallback.
# The BUILD 12 pipeline-admission decision has already happened before this
# function is consulted by GetGraphicsPipeline(), so returning false here for
# Driveclub changes only flat-vs-native fragment implementation.
$Path = 'src/video_core/safe_gpu/safe_gpu.cpp'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$Signature = 'bool SafeGpuGate::ShouldUseFlatFragment(const std::uint64_t pipeline_hash) noexcept {'
$Start = $Text.IndexOf($Signature)
if ($Start -lt 0 -or $Text.IndexOf($Signature, $Start + 1) -ge 0) {
    throw 'Build 14 transform failed: ShouldUseFlatFragment signature was not found exactly once'
}

$OpenBrace = $Text.IndexOf('{', $Start)
if ($OpenBrace -lt 0) {
    throw 'Build 14 transform failed: ShouldUseFlatFragment opening brace was not found'
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
    throw 'Build 14 transform failed: ShouldUseFlatFragment closing brace was not found'
}

$OldFunction = $Text.Substring($Start, $End - $Start + 1)
if (-not $OldFunction.Contains('BUILD 13 native-depthless seed')) {
    throw 'Build 14 transform failed: expected BUILD 13 seed logic is absent'
}
if ($OldFunction.Contains('BUILD 14 native-all-admitted-depthless')) {
    throw 'Build 14 transform failed: BUILD 14 logic is already present'
}

$NewFunction = @'
bool SafeGpuGate::ShouldUseFlatFragment(const std::uint64_t pipeline_hash) noexcept {
    if (GetEffectiveMode() != EffectiveGpuMode::SafeGPU ||
        IsKnownControlGraphicsPipelineHashImpl(pipeline_hash)) {
        return false;
    }
    if (active_profile == SafeGpuProfile::Driveclub &&
        IsDriveclubNativeSafeGraphicsPipelineHash(pipeline_hash)) {
        return false;
    }
    if (active_profile == SafeGpuProfile::Driveclub) {
        // BUILD 14 native-all-admitted-depthless: GetGraphicsPipeline has already applied
        // BUILD 12's strict actual depth/stencil attachment admission gate. Therefore every
        // Driveclub pipeline that reaches this decision is already admitted. Keep its original
        // guest fragment shader and fragment resources without widening the admission set.
        return false;
    }
    return true;
}
'@

$Text = $Text.Substring(0, $Start) + $NewFunction + $Text.Substring($End + 1)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Fail closed: verify the replacement exactly once and preserve non-Driveclub fallback behavior.
$Verify = Normalize([System.IO.File]::ReadAllText($Path))
$Marker = 'BUILD 14 native-all-admitted-depthless'
$MarkerIndex = $Verify.IndexOf($Marker)
if ($MarkerIndex -lt 0 -or $Verify.IndexOf($Marker, $MarkerIndex + 1) -ge 0) {
    throw 'Build 14 transform failed verification: BUILD 14 marker is not unique'
}
$VerifyStart = $Verify.IndexOf($Signature)
$VerifyEndMarker = $Verify.IndexOf("`n}", $VerifyStart)
if ($VerifyStart -lt 0 -or $VerifyEndMarker -lt 0) {
    throw 'Build 14 transform failed verification: rewritten function cannot be located'
}
if (-not $Verify.Substring($VerifyStart, $VerifyEndMarker - $VerifyStart + 2).Contains('return true;')) {
    throw 'Build 14 transform failed verification: non-Driveclub flat-fragment fallback was lost'
}

Write-Host 'Build 14 transform verified: every already-admitted Driveclub pipeline uses native guest fragment shaders/resources; admission rules unchanged.'
