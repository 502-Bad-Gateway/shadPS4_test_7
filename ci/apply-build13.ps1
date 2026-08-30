$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Normalize([string]$Text) {
    return $Text.Replace("`r`n", "`n")
}

# BUILD 13 is deliberately a one-axis experiment on top of BUILD 12.
# It admits no new graphics pipelines. For the exact eight Driveclub depthless pipelines
# that BUILD 12 already executes, disable the flat-fragment substitution so the original
# guest fragment shader and guest fragment resources are used through BUILD 10's regular
# descriptor-set path. Every other SafeGPU policy decision remains unchanged.

# Runtime policy identity.
$Path = 'src/video_core/safe_gpu/safe_gpu.h'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$OldPolicy = 'return "milestone-4-strict-flat-no-depth-attachment-v1";'
$NewPolicy = 'return "milestone-5-driveclub-native-depthless-seed-v1";'
if (($Text.Split($OldPolicy).Count - 1) -ne 1) {
    throw 'Build 13 transform failed: BUILD 12 policy identity was not found exactly once'
}
$Text = $Text.Replace($OldPolicy, $NewPolicy)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

$Path = 'src/video_core/safe_gpu/safe_gpu.cpp'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$Old = @'
bool SafeGpuGate::ShouldUseFlatFragment(const std::uint64_t pipeline_hash) noexcept {
    if (GetEffectiveMode() != EffectiveGpuMode::SafeGPU ||
        IsKnownControlGraphicsPipelineHashImpl(pipeline_hash)) {
        return false;
    }
    if (active_profile == SafeGpuProfile::Driveclub &&
        IsDriveclubNativeSafeGraphicsPipelineHash(pipeline_hash)) {
        return false;
    }
    return true;
}
'@
$New = @'
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
        // BUILD 13 native-depthless seed: these eight hashes were already admitted and submitted
        // without a crash in BUILD 12, but were rendered through the constant-color replacement
        // fragment shader. Keep the BUILD 12 admission boundary unchanged and restore only their
        // guest fragment shader/resource path.
        switch (pipeline_hash) {
        case 0xf262db18a573b11bULL:
        case 0xd6662ec1afe89d73ULL:
        case 0x64dc60f23cf56ca6ULL:
        case 0x0066e5d405cec7dcULL:
        case 0xa6bbc4863c4524d0ULL:
        case 0x1870824d8db3a02cULL:
        case 0xbb3816e752b04345ULL:
        case 0x8ef11f7e6600d87aULL:
            return false;
        default:
            break;
        }
    }
    return true;
}
'@
if (($Text.Split($Old).Count - 1) -ne 1) {
    throw 'Build 13 transform failed: BUILD 11/12 ShouldUseFlatFragment body was not found exactly once'
}
$Text = $Text.Replace($Old, $New)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Fail-closed verification: each seed hash must occur exactly once in the transformed source.
$Verify = Normalize([System.IO.File]::ReadAllText($Path))
$Seeds = @(
    '0xf262db18a573b11bULL',
    '0xd6662ec1afe89d73ULL',
    '0x64dc60f23cf56ca6ULL',
    '0x0066e5d405cec7dcULL',
    '0xa6bbc4863c4524d0ULL',
    '0x1870824d8db3a02cULL',
    '0xbb3816e752b04345ULL',
    '0x8ef11f7e6600d87aULL'
)
foreach ($Seed in $Seeds) {
    if (($Verify.Split($Seed).Count - 1) -ne 1) {
        throw "Build 13 transform failed verification for seed $Seed"
    }
}

Write-Host 'Build 13 transform verified: eight already-admitted Driveclub depthless pipelines use native fragment shaders/resources.'
