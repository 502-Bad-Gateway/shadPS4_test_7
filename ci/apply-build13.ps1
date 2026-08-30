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
$PolicyIndex = $Text.IndexOf($OldPolicy, [System.StringComparison]::Ordinal)
if ($PolicyIndex -lt 0 -or
    $Text.IndexOf($OldPolicy, $PolicyIndex + $OldPolicy.Length, [System.StringComparison]::Ordinal) -ge 0) {
    throw 'Build 13 transform failed: BUILD 12 policy identity was not found exactly once'
}
$Text = $Text.Replace($OldPolicy, $NewPolicy)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Locate ShouldUseFlatFragment by function boundaries instead of matching its complete body.
# BUILD 11 owns the existing Driveclub native-safe-hash exception. BUILD 13 inserts one new
# exception immediately before this function's final return true, without changing any other logic.
$Path = 'src/video_core/safe_gpu/safe_gpu.cpp'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$StartSignature = 'bool SafeGpuGate::ShouldUseFlatFragment(const std::uint64_t pipeline_hash) noexcept {'
$EndSignature = 'bool SafeGpuGate::ShouldAllowGraphicsPipeline('
$Start = $Text.IndexOf($StartSignature, [System.StringComparison]::Ordinal)
if ($Start -lt 0) {
    throw 'Build 13 transform failed: ShouldUseFlatFragment start was not found'
}
$End = $Text.IndexOf($EndSignature, $Start + $StartSignature.Length, [System.StringComparison]::Ordinal)
if ($End -lt 0) {
    throw 'Build 13 transform failed: ShouldUseFlatFragment end boundary was not found'
}
$Segment = $Text.Substring($Start, $End - $Start)
$ReturnNeedle = "    return true;`n}"
$ReturnIndex = $Segment.LastIndexOf($ReturnNeedle, [System.StringComparison]::Ordinal)
if ($ReturnIndex -lt 0) {
    throw 'Build 13 transform failed: final ShouldUseFlatFragment return true was not found'
}

$SeedBlock = @'
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
'@

# Refuse to double-apply if the seed block somehow already exists.
if ($Segment.Contains('BUILD 13 native-depthless seed')) {
    throw 'Build 13 transform failed: native-depthless seed block already present before transform'
}
$Segment = $Segment.Substring(0, $ReturnIndex) + $SeedBlock +
           $Segment.Substring($ReturnIndex)
$Text = $Text.Substring(0, $Start) + $Segment + $Text.Substring($End)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Fail-closed verification: each seed hash must occur exactly once inside ShouldUseFlatFragment,
# the BUILD 11 native-safe exception must still exist, and the final fallback must still be true.
$Verify = Normalize([System.IO.File]::ReadAllText($Path))
$VerifyStart = $Verify.IndexOf($StartSignature, [System.StringComparison]::Ordinal)
$VerifyEnd = $Verify.IndexOf($EndSignature, $VerifyStart + $StartSignature.Length,
                            [System.StringComparison]::Ordinal)
if ($VerifyStart -lt 0 -or $VerifyEnd -lt 0) {
    throw 'Build 13 transform failed verification: function boundaries disappeared'
}
$VerifySegment = $Verify.Substring($VerifyStart, $VerifyEnd - $VerifyStart)
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
    $First = $VerifySegment.IndexOf($Seed, [System.StringComparison]::Ordinal)
    $Second = if ($First -ge 0) {
        $VerifySegment.IndexOf($Seed, $First + $Seed.Length, [System.StringComparison]::Ordinal)
    } else {
        -1
    }
    if ($First -lt 0 -or $Second -ge 0) {
        throw "Build 13 transform failed verification for seed $Seed"
    }
}
if (-not $VerifySegment.Contains('IsDriveclubNativeSafeGraphicsPipelineHash(pipeline_hash)') -or
    -not $VerifySegment.Contains("    return true;`n}")) {
    throw 'Build 13 transform failed verification: BUILD 11 behavior or final fallback was altered'
}

Write-Host 'Build 13 transform verified: eight already-admitted Driveclub depthless pipelines use native fragment shaders/resources.'
