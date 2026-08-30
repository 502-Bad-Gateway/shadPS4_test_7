$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Normalize([string]$Text) {
    return $Text.Replace("`r`n", "`n")
}

# BUILD 16 is an exact-hash graphics-prefix experiment on top of BUILD 15.
# It admits only three Driveclub graphics pipelines observed in the FullGPU prefix
# and encountered/rejected by SafeGPU. No generic structural rule is relaxed.
# BUILD 14 native fragment/resource handling, BUILD 15 proven-depth restoration,
# BUILD 10 regular descriptor sets, and compute suppression remain unchanged.

# Runtime policy identity.
$Path = 'src/video_core/safe_gpu/safe_gpu.h'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$OldPolicy = 'return "milestone-7-driveclub-restore-proven-depth-v1";'
$NewPolicy = 'return "milestone-8-driveclub-exact-fullgpu-prefix-gaps-v1";'
$PolicyIndex = $Text.IndexOf($OldPolicy)
if ($PolicyIndex -lt 0 -or $Text.IndexOf($OldPolicy, $PolicyIndex + 1) -ge 0) {
    throw 'Build 16 transform failed: BUILD 15 policy identity was not found exactly once'
}
$Text = $Text.Substring(0, $PolicyIndex) + $NewPolicy +
        $Text.Substring($PolicyIndex + $OldPolicy.Length)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Structurally locate ShouldAllowGraphicsPipeline and insert the exact Driveclub
# exception before common_simple. These hashes otherwise fail the generic structural
# classifier because they exercise geometry, MRT/depth-only layouts, respectively.
$Path = 'src/video_core/safe_gpu/safe_gpu.cpp'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$Signature = 'bool SafeGpuGate::ShouldAllowGraphicsPipeline('
$Start = $Text.IndexOf($Signature)
if ($Start -lt 0 -or $Text.IndexOf($Signature, $Start + 1) -ge 0) {
    throw 'Build 16 transform failed: ShouldAllowGraphicsPipeline signature was not found exactly once'
}

$OpenBrace = $Text.IndexOf('{', $Start)
if ($OpenBrace -lt 0) {
    throw 'Build 16 transform failed: ShouldAllowGraphicsPipeline opening brace was not found'
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
    throw 'Build 16 transform failed: ShouldAllowGraphicsPipeline closing brace was not found'
}

$Function = $Text.Substring($Start, $End - $Start + 1)
if ($Function.Contains('BUILD 16 exact FullGPU-prefix gaps')) {
    throw 'Build 16 transform failed: BUILD 16 logic is already present'
}

$Anchor = '    const bool common_simple ='
$AnchorIndex = $Function.IndexOf($Anchor)
if ($AnchorIndex -lt 0 -or $Function.IndexOf($Anchor, $AnchorIndex + 1) -ge 0) {
    throw 'Build 16 transform failed: common_simple anchor was not found exactly once'
}

$Insert = @'
    if (active_profile == SafeGpuProfile::Driveclub) {
        // BUILD 16 exact FullGPU-prefix gaps. These are the only newly admitted hashes:
        // 0x8c2670964bfb9597: vertex + geometry + fragment, PointList
        // 0x4825a6db38d25ca9: vertex + fragment, four-MRT pipeline layout
        // 0xe23a86ace3fa0129: vertex + fragment, D16 depth-only pipeline
        // All were created successfully in the bounded Driveclub FullGPU prefix and are
        // encountered by SafeGPU. Do not relax the classifier for any other hash.
        switch (info.pipeline_hash) {
        case 0x8c2670964bfb9597ULL:
        case 0x4825a6db38d25ca9ULL:
        case 0xe23a86ace3fa0129ULL:
            return true;
        default:
            break;
        }
    }

'@
$Function = $Function.Substring(0, $AnchorIndex) + $Insert +
            $Function.Substring($AnchorIndex)
$Text = $Text.Substring(0, $Start) + $Function + $Text.Substring($End + 1)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Fail closed: verify marker and all three hashes are unique in the transformed function.
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
    throw 'Build 16 transform failed verification: transformed function boundary is invalid'
}
$VerifyFunction = $Verify.Substring($VerifyStart, $VerifyEnd - $VerifyStart + 1)
$Marker = 'BUILD 16 exact FullGPU-prefix gaps'
$MarkerIndex = $VerifyFunction.IndexOf($Marker)
if ($MarkerIndex -lt 0 -or $VerifyFunction.IndexOf($Marker, $MarkerIndex + 1) -ge 0) {
    throw 'Build 16 transform failed verification: BUILD 16 marker is not unique'
}
foreach ($Hash in @('0x8c2670964bfb9597ULL', '0x4825a6db38d25ca9ULL', '0xe23a86ace3fa0129ULL')) {
    $HashIndex = $VerifyFunction.IndexOf($Hash)
    if ($HashIndex -lt 0 -or $VerifyFunction.IndexOf($Hash, $HashIndex + 1) -ge 0) {
        throw "Build 16 transform failed verification: exact hash $Hash is not unique"
    }
}

Write-Host 'Build 16 transform verified: exactly three Driveclub FullGPU-prefix gap pipelines admitted; all other SafeGPU restrictions unchanged.'
