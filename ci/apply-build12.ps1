$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Normalize([string]$Text) {
    return $Text.Replace("`r`n", "`n")
}

# BUILD 12 is deliberately a one-axis experiment on top of the complete BUILD 11 transform.
# BUILD 11 classified "depthless" from PS4 depth/stencil test enable bits. The failing BUILD 11
# captures showed that a flat-fragment-substituted pipeline could still carry a real Vulkan
# depth/stencil attachment (D32SfloatS8Uint). BUILD 12 changes only that classification boundary:
# for the ordinary flat-fragment path, any actual depth OR stencil attachment makes the candidate
# unsafe. Driveclub's exact native-safe hashes remain handled by BUILD 11 before this fallback gate.

# Give runtime logs an unambiguous policy identity.
$Path = 'src/video_core/safe_gpu/safe_gpu.h'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$OldPolicy = 'return "milestone-3-per-title-quarantine-v1";'
$NewPolicy = 'return "milestone-4-strict-flat-no-depth-attachment-v1";'
if (($Text.Split($OldPolicy).Count - 1) -ne 1) {
    throw 'Build 12 transform failed: BUILD 11 policy identity was not found exactly once'
}
$Text = $Text.Replace($OldPolicy, $NewPolicy)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Change the authoritative pre-vkCreateGraphicsPipelines SafeGPU classifier to describe actual
# attachments recorded in GraphicsPipelineKey, not merely whether depth/stencil testing is enabled.
$Path = 'src/video_core/renderer_vulkan/vk_pipeline_cache.cpp'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$Pattern = '(?ms)\.has_depth\s*=\s*liverpool->regs\.depth_buffer\.DepthValid\(\)\s*&&\s*liverpool->regs\.depth_control\.depth_enable\s*!=\s*0,\s*\.has_stencil\s*=\s*liverpool->regs\.depth_control\.stencil_enable\s*!=\s*0,'
$Matches = [regex]::Matches($Text, $Pattern)
if ($Matches.Count -ne 1) {
    throw "Build 12 transform failed: expected one BUILD 11 SafeGPU depth/stencil classifier, found $($Matches.Count)"
}
$Replacement = @'
.has_depth =
            graphics_key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid,
        .has_stencil =
            graphics_key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid,
'@
$Text = [regex]::Replace($Text, $Pattern, $Replacement, 1)

# Logging-only labels. These do not change the experiment axis; they make captured logs self-identifying.
$Text = $Text.Replace(
    'Build 06 geometry-first feature gate rejected it before vkCreateGraphicsPipelines',
    'Build 12 strict no-depth-attachment gate rejected it before vkCreateGraphicsPipelines')
$Text = $Text.Replace(
    'Build 06 candidate prefilter and geometry-first feature gate before vkCreateGraphicsPipelines',
    'Build 12 candidate prefilter and strict no-depth-attachment gate before vkCreateGraphicsPipelines')

[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Fail closed if the intended BUILD 12 expressions are not present exactly once after transformation.
$Verify = Normalize([System.IO.File]::ReadAllText($Path))
$DepthExpr = 'graphics_key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid'
$StencilExpr = 'graphics_key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid'
if (($Verify.Split($DepthExpr).Count - 1) -ne 1 -or
    ($Verify.Split($StencilExpr).Count - 1) -ne 1) {
    throw 'Build 12 transform failed verification: strict attachment predicates are not unique'
}

Write-Host 'Build 12 transform verified: flat-fragment candidates now require no actual depth/stencil attachment.'
