$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$Path = 'src/video_core/renderer_vulkan/vk_rasterizer.cpp'
$Text = [System.IO.File]::ReadAllText($Path).Replace("`r`n", "`n")

# apply-build11.ps1 may have inserted both provisional markers at the first graphics bind.
# Remove every provisional Build 11 marker, then reinsert exactly one marker inside each
# graphics draw function by function boundaries rather than surrounding formatting.
$Text = [regex]::Replace(
    $Text,
    '(?m)^[ \t]*PersistLastSubmittedGraphicsPipeline\(pipeline, "(?:direct|indirect)", is_indexed\);\n?',
    '')

$Bind = '    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());'

function Insert-In-Function([string]$Source, [string]$StartSignature,
                            [string]$EndSignature, [string]$DrawType) {
    $Start = $Source.IndexOf($StartSignature)
    if ($Start -lt 0) {
        throw "Build 11 draw-site fix failed: function start not found: $StartSignature"
    }

    $End = $Source.IndexOf($EndSignature, $Start + $StartSignature.Length)
    if ($End -lt 0) {
        throw "Build 11 draw-site fix failed: function end not found after: $StartSignature"
    }

    $Segment = $Source.Substring($Start, $End - $Start)
    $BindIndex = $Segment.IndexOf($Bind)
    if ($BindIndex -lt 0) {
        throw "Build 11 draw-site fix failed: graphics bind not found inside: $StartSignature"
    }

    if ($Segment.IndexOf($Bind, $BindIndex + $Bind.Length) -ge 0) {
        throw "Build 11 draw-site fix failed: multiple graphics binds inside: $StartSignature"
    }

    $Marker = $Bind + "`n    PersistLastSubmittedGraphicsPipeline(pipeline, `"$DrawType`", is_indexed);"
    $Segment = $Segment.Substring(0, $BindIndex) + $Marker +
               $Segment.Substring($BindIndex + $Bind.Length)

    return $Source.Substring(0, $Start) + $Segment + $Source.Substring($End)
}

$Text = Insert-In-Function $Text 'void Rasterizer::Draw(bool is_indexed, u32 index_offset)' `
                                'void Rasterizer::DrawIndirect(' 'direct'
$Text = Insert-In-Function $Text 'void Rasterizer::DrawIndirect(' `
                                'void Rasterizer::DispatchDirect()' 'indirect'

$DirectCount = ([regex]::Matches(
    $Text,
    'PersistLastSubmittedGraphicsPipeline\(pipeline, "direct", is_indexed\);')).Count
$IndirectCount = ([regex]::Matches(
    $Text,
    'PersistLastSubmittedGraphicsPipeline\(pipeline, "indirect", is_indexed\);')).Count
if ($DirectCount -ne 1 -or $IndirectCount -ne 1) {
    throw "Build 11 draw-site fix failed verification: direct=$DirectCount indirect=$IndirectCount"
}

[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
Write-Host 'Build 11 persistent draw-site placement verified: direct=1 indirect=1.'
