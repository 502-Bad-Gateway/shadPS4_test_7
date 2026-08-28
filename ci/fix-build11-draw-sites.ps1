$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$Path = 'src/video_core/renderer_vulkan/vk_rasterizer.cpp'
$Text = [System.IO.File]::ReadAllText($Path).Replace("`r`n", "`n")

# Make this pass independent of any intermediate placement produced by apply-build11.ps1.
# Remove all existing Build 11 draw-site calls, then add exactly one direct and one indirect marker.
$Text = $Text.Replace("    PersistLastSubmittedGraphicsPipeline(pipeline, `"direct`", is_indexed);`n", "")
$Text = $Text.Replace("    PersistLastSubmittedGraphicsPipeline(pipeline, `"indirect`", is_indexed);`n", "")

$DirectOld = @'
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
'@
$DirectNew = @'
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
    PersistLastSubmittedGraphicsPipeline(pipeline, "direct", is_indexed);

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
'@
if (-not $Text.Contains($DirectOld)) {
    throw 'Build 11 draw-site fix failed: unique direct graphics draw site not found'
}
$Text = $Text.Replace($DirectOld, $DirectNew)

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
    throw 'Build 11 draw-site fix failed: unique indirect graphics draw site not found'
}
$Text = $Text.Replace($IndirectOld, $IndirectNew)

[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
Write-Host 'Build 11 persistent draw-site placement verified: one direct and one indirect site.'
