$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Normalize([string]$Text) {
    return $Text.Replace("`r`n", "`n")
}

# BUILD 16-r2 corrects an implementation gap in BUILD 16 without changing its intended axis.
# BUILD 16 admitted exactly three Driveclub FullGPU-prefix hashes at PipelineCache's authoritative
# pre-vkCreateGraphicsPipelines classifier. Runtime evidence showed Vulkan created all three, but
# Rasterizer::IsSafeGpuGraphicsPipeline still used the older aggregate structural recheck and
# rejected at least the geometry and D16 candidates before draw submission. BUILD 16-r2 teaches
# only that post-create recheck about the same three exact hashes. No other pipeline is widened.

# Runtime policy identity.
$Path = 'src/video_core/safe_gpu/safe_gpu.h'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$OldPolicy = 'return "milestone-8-driveclub-exact-fullgpu-prefix-gaps-v1";'
$NewPolicy = 'return "milestone-8r2-driveclub-exact-fullgpu-prefix-gaps-postcreate-v1";'
$Index = $Text.IndexOf($OldPolicy)
if ($Index -lt 0 -or $Text.IndexOf($OldPolicy, $Index + 1) -ge 0) {
    throw 'Build 16-r2 transform failed: BUILD 16 policy identity was not found exactly once'
}
$Text = $Text.Substring(0, $Index) + $NewPolicy + $Text.Substring($Index + $OldPolicy.Length)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Structurally locate Rasterizer::IsSafeGpuGraphicsPipeline and add an exact-hash bypass before
# the legacy aggregate classifier. This bypass is Driveclub-only and contains exactly the same
# three hashes admitted by BUILD 16 at PipelineCache.
$Path = 'src/video_core/renderer_vulkan/vk_rasterizer.cpp'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$Signature = 'bool Rasterizer::IsSafeGpuGraphicsPipeline(const GraphicsPipeline* pipeline) const {'
$Start = $Text.IndexOf($Signature)
if ($Start -lt 0 -or $Text.IndexOf($Signature, $Start + 1) -ge 0) {
    throw 'Build 16-r2 transform failed: IsSafeGpuGraphicsPipeline signature was not found exactly once'
}
$OpenBrace = $Text.IndexOf('{', $Start)
if ($OpenBrace -lt 0) {
    throw 'Build 16-r2 transform failed: IsSafeGpuGraphicsPipeline opening brace was not found'
}
$Depth = 0
$End = -1
for ($i = $OpenBrace; $i -lt $Text.Length; ++$i) {
    if ($Text[$i] -eq '{') { ++$Depth }
    elseif ($Text[$i] -eq '}') {
        --$Depth
        if ($Depth -eq 0) { $End = $i; break }
    }
}
if ($End -lt 0) {
    throw 'Build 16-r2 transform failed: IsSafeGpuGraphicsPipeline closing brace was not found'
}
$Function = $Text.Substring($Start, $End - $Start + 1)
if ($Function.Contains('BUILD 16-r2 exact post-create gap bypass')) {
    throw 'Build 16-r2 transform failed: correction is already present'
}

$Anchor = '    const auto& key = pipeline->GetGraphicsKey();'
$AnchorIndex = $Function.IndexOf($Anchor)
if ($AnchorIndex -lt 0 -or $Function.IndexOf($Anchor, $AnchorIndex + 1) -ge 0) {
    throw 'Build 16-r2 transform failed: GraphicsPipelineKey anchor was not found exactly once'
}
$InsertAt = $AnchorIndex + $Anchor.Length
$Insert = @'


    // BUILD 16-r2 exact post-create gap bypass. PipelineCache already admitted exactly these
    // three Driveclub hashes in BUILD 16 and Vulkan created them successfully. The older
    // Rasterizer aggregate classifier cannot faithfully represent their geometry/MRT/D16 state,
    // so allow these exact same hashes through the second gate and nothing else.
    if (VideoCore::SafeGpuGate::GetProfile() == VideoCore::SafeGpuProfile::Driveclub) {
        const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(key);
        switch (pipeline_hash) {
        case 0x8c2670964bfb9597ULL:
        case 0x4825a6db38d25ca9ULL:
        case 0xe23a86ace3fa0129ULL:
            return true;
        default:
            break;
        }
    }
'@
$Function = $Function.Substring(0, $InsertAt) + $Insert + $Function.Substring($InsertAt)
$Text = $Text.Substring(0, $Start) + $Function + $Text.Substring($End + 1)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Fail closed verification.
$Verify = Normalize([System.IO.File]::ReadAllText($Path))
$VerifyStart = $Verify.IndexOf($Signature)
$VerifyOpen = $Verify.IndexOf('{', $VerifyStart)
$VerifyDepth = 0
$VerifyEnd = -1
for ($i = $VerifyOpen; $i -lt $Verify.Length; ++$i) {
    if ($Verify[$i] -eq '{') { ++$VerifyDepth }
    elseif ($Verify[$i] -eq '}') {
        --$VerifyDepth
        if ($VerifyDepth -eq 0) { $VerifyEnd = $i; break }
    }
}
if ($VerifyEnd -lt 0) {
    throw 'Build 16-r2 transform failed verification: transformed function boundary is invalid'
}
$VerifyFunction = $Verify.Substring($VerifyStart, $VerifyEnd - $VerifyStart + 1)
$Marker = 'BUILD 16-r2 exact post-create gap bypass'
$MarkerIndex = $VerifyFunction.IndexOf($Marker)
if ($MarkerIndex -lt 0 -or $VerifyFunction.IndexOf($Marker, $MarkerIndex + 1) -ge 0) {
    throw 'Build 16-r2 transform failed verification: correction marker is not unique'
}
foreach ($Hash in @('0x8c2670964bfb9597ULL', '0x4825a6db38d25ca9ULL', '0xe23a86ace3fa0129ULL')) {
    $HashIndex = $VerifyFunction.IndexOf($Hash)
    if ($HashIndex -lt 0 -or $VerifyFunction.IndexOf($Hash, $HashIndex + 1) -ge 0) {
        throw "Build 16-r2 transform failed verification: exact hash $Hash is not unique in post-create gate"
    }
}

Write-Host 'Build 16-r2 transform verified: the same three Driveclub BUILD 16 hashes bypass only the legacy post-create structural recheck.'
