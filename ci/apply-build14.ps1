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

# Structurally locate SafeGpuGate::ShouldUseFlatFragment after BUILD 13, then
# insert the BUILD 14 Driveclub-wide native-fragment decision immediately
# before the function's final fallback `return true;`. The BUILD 12 pipeline
# gate has already run before this function is consulted by GetGraphicsPipeline,
# so this changes fragment implementation only; it cannot admit a rejected
# pipeline.
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

$Function = $Text.Substring($Start, $End - $Start + 1)
if (-not $Function.Contains('BUILD 13 native-depthless seed')) {
    throw 'Build 14 transform failed: expected BUILD 13 seed logic is absent'
}
if ($Function.Contains('BUILD 14 native-all-admitted-depthless')) {
    throw 'Build 14 transform failed: BUILD 14 logic is already present'
}

$Fallback = "`n    return true;"
$FallbackIndex = $Function.LastIndexOf($Fallback)
if ($FallbackIndex -lt 0) {
    throw 'Build 14 transform failed: final ShouldUseFlatFragment fallback was not found'
}

$Insert = @'

    if (active_profile == SafeGpuProfile::Driveclub) {
        // BUILD 14 native-all-admitted-depthless: GetGraphicsPipeline has already applied
        // BUILD 12's strict actual depth/stencil attachment admission gate. Therefore any
        // remaining Driveclub pipeline reaching this decision is already admitted; restore
        // its original guest fragment shader and original fragment resources without widening
        // the set of allowed pipelines.
        return false;
    }
'@
$Function = $Function.Substring(0, $FallbackIndex) + $Insert +
            $Function.Substring($FallbackIndex)
$Text = $Text.Substring(0, $Start) + $Function + $Text.Substring($End + 1)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Fail closed: verify the marker and policy each occur exactly once.
$Verify = Normalize([System.IO.File]::ReadAllText($Path))
$Marker = 'BUILD 14 native-all-admitted-depthless'
$MarkerIndex = $Verify.IndexOf($Marker)
if ($MarkerIndex -lt 0 -or $Verify.IndexOf($Marker, $MarkerIndex + 1) -ge 0) {
    throw 'Build 14 transform failed verification: BUILD 14 marker is not unique'
}

Write-Host 'Build 14 transform verified: every already-admitted Driveclub pipeline uses native guest fragment shaders/resources; admission rules unchanged.'
