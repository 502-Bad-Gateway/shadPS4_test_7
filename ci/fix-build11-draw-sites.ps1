$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$Path = 'src/video_core/renderer_vulkan/vk_rasterizer.cpp'
$Text = [System.IO.File]::ReadAllText($Path).Replace("`r`n", "`n")

$BadDirect = @'
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
    PersistLastSubmittedGraphicsPipeline(pipeline, "indirect", is_indexed);
    PersistLastSubmittedGraphicsPipeline(pipeline, "direct", is_indexed);
'@
$GoodDirect = @'
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
    PersistLastSubmittedGraphicsPipeline(pipeline, "direct", is_indexed);
'@
if (-not $Text.Contains($BadDirect)) {
    throw 'Build 11 draw-site fix failed: doubled direct diagnostic site not found'
}
$Text = $Text.Replace($BadDirect, $GoodDirect)

$IndirectOld = @'
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);
'@
$IndirectNew = @'
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
    PersistLastSubmittedGraphicsPipeline(pipeline, "indirect", is_indexed);

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);
'@
if (-not $Text.Contains($IndirectOld)) {
    throw 'Build 11 draw-site fix failed: indirect graphics draw site not found'
}
$Text = $Text.Replace($IndirectOld, $IndirectNew)

[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
Write-Host 'Build 11 persistent draw-site placement verified.'
