#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8-sig")


def write(rel, text):
    path = ROOT / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


# -----------------------------------------------------------------------------
# Settings: persistent three-state GPU mode (Normal / Dumb / Null)
# -----------------------------------------------------------------------------
path = "src/core/emulator_settings.h"
s = read(path)
s = replace_once(
    s,
    "    Setting<bool> null_gpu{false};\n    Setting<bool> copy_gpu_buffers{false};",
    "    Setting<bool> null_gpu{false};\n"
    "    // Best-effort renderer: keep the real Vulkan device/rasterizer active, but allow\n"
    "    // unsupported guest GPU work to be skipped instead of aborting the emulator.\n"
    "    Setting<bool> dumb_gpu{false};\n"
    "    Setting<bool> copy_gpu_buffers{false};",
    "GPUSettings.dumb_gpu field",
)
s = replace_once(
    s,
    "            make_override<GPUSettings>(\"null_gpu\", &GPUSettings::null_gpu),\n"
    "            make_override<GPUSettings>(\"copy_gpu_buffers\", &GPUSettings::copy_gpu_buffers),",
    "            make_override<GPUSettings>(\"null_gpu\", &GPUSettings::null_gpu),\n"
    "            make_override<GPUSettings>(\"dumb_gpu\", &GPUSettings::dumb_gpu),\n"
    "            make_override<GPUSettings>(\"copy_gpu_buffers\", &GPUSettings::copy_gpu_buffers),",
    "GPUSettings dumb override",
)
s = replace_once(
    s,
    "NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GPUSettings, window_width, window_height, internal_screen_width,\n"
    "                                   internal_screen_height, null_gpu, copy_gpu_buffers,",
    "// WITH_DEFAULT keeps existing config files valid when the new dumb_gpu key is absent.\n"
    "NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(\n"
    "    GPUSettings, window_width, window_height, internal_screen_width, internal_screen_height,\n"
    "    null_gpu, dumb_gpu, copy_gpu_buffers,",
    "GPUSettings JSON compatibility",
)
s = replace_once(
    s,
    "                                   hdr_allowed, fsr_enabled, rcas_enabled, rcas_attenuation)\n",
    "    hdr_allowed, fsr_enabled, rcas_enabled, rcas_attenuation)\n",
    "GPUSettings JSON formatting tail",
)
s = replace_once(
    s,
    "    SETTING_FORWARD_BOOL(m_gpu, NullGPU, null_gpu)\n"
    "    SETTING_FORWARD_BOOL(m_gpu, DumpShaders, dump_shaders)",
    "    SETTING_FORWARD_BOOL(m_gpu, NullGPU, null_gpu)\n"
    "    SETTING_FORWARD_BOOL(m_gpu, DumbGPU, dumb_gpu)\n"
    "    SETTING_FORWARD_BOOL(m_gpu, DumpShaders, dump_shaders)",
    "DumbGPU accessor",
)
write(path, s)


# -----------------------------------------------------------------------------
# Big-picture settings UI: one mutually-exclusive compatibility combo.
# -----------------------------------------------------------------------------
path = "src/imgui/big_picture/settings_dialog_imgui.h"
s = read(path)
s = replace_once(
    s,
    "    const std::vector<std::string> presentModeOptions = {\"Mailbox\", \"Fifo\", \"Immediate\"};\n",
    "    const std::vector<std::string> presentModeOptions = {\"Mailbox\", \"Fifo\", \"Immediate\"};\n"
    "    const std::vector<std::string> gpuCompatibilityModeOptions = {\n"
    "        \"Normal GPU\", \"Dumb GPU (Best Effort)\", \"Null GPU (No Guest Graphics)\"};\n",
    "GPU compatibility UI options",
)
s = replace_once(
    s,
    "    int presentModeSetting;\n    int windowWidthSetting;",
    "    int presentModeSetting;\n    int gpuCompatibilityModeSetting;\n    int windowWidthSetting;",
    "GPU compatibility UI state",
)
write(path, s)

path = "src/imgui/big_picture/settings_dialog_imgui.cpp"
s = read(path)
s = replace_once(
    s,
    "    presentModeSetting = GetComboIndex(EmulatorSettings.GetPresentMode(), presentModeOptions);\n"
    "    windowHeightSetting = EmulatorSettings.GetWindowHeight();",
    "    presentModeSetting = GetComboIndex(EmulatorSettings.GetPresentMode(), presentModeOptions);\n"
    "    gpuCompatibilityModeSetting = EmulatorSettings.IsNullGPU()\n"
    "                                      ? 2\n"
    "                                      : (EmulatorSettings.IsDumbGPU() ? 1 : 0);\n"
    "    windowHeightSetting = EmulatorSettings.GetWindowHeight();",
    "Load GPU compatibility mode",
)
s = replace_once(
    s,
    "    EmulatorSettings.SetPresentMode(presentModeOptions.at(presentModeSetting), isSpecific);\n"
    "    EmulatorSettings.SetWindowHeight(windowHeightSetting, isSpecific);",
    "    EmulatorSettings.SetPresentMode(presentModeOptions.at(presentModeSetting), isSpecific);\n"
    "    // Null GPU wins over Dumb GPU; the combo always writes a mutually-exclusive state.\n"
    "    EmulatorSettings.SetDumbGPU(gpuCompatibilityModeSetting == 1, isSpecific);\n"
    "    EmulatorSettings.SetNullGPU(gpuCompatibilityModeSetting == 2, isSpecific);\n"
    "    EmulatorSettings.SetWindowHeight(windowHeightSetting, isSpecific);",
    "Save GPU compatibility mode",
)
s = replace_once(
    s,
    "            AddSettingCombo(\"Present Mode\", presentModeSetting, presentModeOptions);\n"
    "            AddSettingSliderInt(\"Window Width\", windowWidthSetting, 0, 8000);",
    "            AddSettingCombo(\"Present Mode\", presentModeSetting, presentModeOptions);\n"
    "            AddSettingCombo(\"GPU Compatibility Mode (Requires Restart)\",\n"
    "                            gpuCompatibilityModeSetting, gpuCompatibilityModeOptions);\n"
    "            AddSettingSliderInt(\"Window Width\", windowWidthSetting, 0, 8000);",
    "Draw GPU compatibility mode",
)
write(path, s)


# -----------------------------------------------------------------------------
# Runtime diagnostics.
# -----------------------------------------------------------------------------
path = "src/emulator.cpp"
s = read(path)
s = replace_once(
    s,
    "    LOG_INFO(Config, \"GPU isNullGpu: {}\", EmulatorSettings.IsNullGPU());\n"
    "    LOG_INFO(Config, \"GPU readbacksMode: {}\", EmulatorSettings.GetReadbacksMode());",
    "    LOG_INFO(Config, \"GPU isNullGpu: {}\", EmulatorSettings.IsNullGPU());\n"
    "    LOG_INFO(Config, \"GPU isDumbGpu: {}\", EmulatorSettings.IsDumbGPU());\n"
    "    LOG_INFO(Config, \"GPU readbacksMode: {}\", EmulatorSettings.GetReadbacksMode());",
    "Dumb GPU runtime log",
)
write(path, s)


# -----------------------------------------------------------------------------
# Vulkan 1.2 -> format_feature_flags2 compatibility.
# The Vulkan 1.3/flags2 effective-format rules add feature bits that cannot be
# represented by legacy VkFormatFeatureFlags. Reconstruct only spec-defined bits;
# do NOT pretend unsupported formats/extensions exist.
# -----------------------------------------------------------------------------
path = "src/video_core/renderer_vulkan/vk_instance.cpp"
s = read(path)
old = '''vk::FormatProperties3 GetFormatProperties(vk::PhysicalDevice physical, vk::Format format) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
    const vk::FormatProperties properties = physical.getFormatProperties(format);
    const auto widen = [](vk::FormatFeatureFlags flags) {
        return vk::FormatFeatureFlags2{
            static_cast<VkFormatFeatureFlags2>(static_cast<VkFormatFeatureFlags>(flags))};
    };
    return vk::FormatProperties3{
        .linearTilingFeatures = widen(properties.linearTilingFeatures),
        .optimalTilingFeatures = widen(properties.optimalTilingFeatures),
        .bufferFeatures = widen(properties.bufferFeatures),
    };
#else
    vk::FormatProperties3 properties3{};
    vk::FormatProperties2 properties2 = {
        .pNext = &properties3,
    };
    physical.getFormatProperties2(format, &properties2);
    return properties3;
#endif
}
'''
new = '''#ifdef SHADPS4_WINDOWS_7_COMPAT
bool IsStorageWithoutFormatFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Snorm:
    case vk::Format::eR8G8B8A8Uint:
    case vk::Format::eR8G8B8A8Sint:
    case vk::Format::eR32Uint:
    case vk::Format::eR32Sint:
    case vk::Format::eR32Sfloat:
    case vk::Format::eR32G32Uint:
    case vk::Format::eR32G32Sint:
    case vk::Format::eR32G32Sfloat:
    case vk::Format::eR32G32B32A32Uint:
    case vk::Format::eR32G32B32A32Sint:
    case vk::Format::eR32G32B32A32Sfloat:
    case vk::Format::eR16G16B16A16Uint:
    case vk::Format::eR16G16B16A16Sint:
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR16G16Sfloat:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eR16Sfloat:
    case vk::Format::eR16G16B16A16Unorm:
    case vk::Format::eA2B10G10R10UnormPack32:
    case vk::Format::eR16G16Unorm:
    case vk::Format::eR8G8Unorm:
    case vk::Format::eR16Unorm:
    case vk::Format::eR8Unorm:
    case vk::Format::eR16G16B16A16Snorm:
    case vk::Format::eR16G16Snorm:
    case vk::Format::eR8G8Snorm:
    case vk::Format::eR16Snorm:
    case vk::Format::eR8Snorm:
    case vk::Format::eR16G16Sint:
    case vk::Format::eR8G8Sint:
    case vk::Format::eR16Sint:
    case vk::Format::eR8Sint:
    case vk::Format::eA2B10G10R10UintPack32:
    case vk::Format::eR16G16Uint:
    case vk::Format::eR8G8Uint:
    case vk::Format::eR16Uint:
    case vk::Format::eR8Uint:
        return true;
    default:
        return false;
    }
}

bool HasDepthComponent(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return true;
    default:
        return false;
    }
}

vk::FormatFeatureFlags2 ExpandLegacyFormatFeatures(vk::Format format,
                                                   vk::FormatFeatureFlags legacy,
                                                   const vk::PhysicalDeviceFeatures& features,
                                                   bool buffer_features) {
    vk::FormatFeatureFlags2 result{
        static_cast<VkFormatFeatureFlags2>(static_cast<VkFormatFeatureFlags>(legacy))};

    if (IsStorageWithoutFormatFormat(format)) {
        const bool storage_capable =
            buffer_features
                ? static_cast<bool>(legacy & vk::FormatFeatureFlagBits::eStorageTexelBuffer)
                : static_cast<bool>(legacy & vk::FormatFeatureFlagBits::eStorageImage);
        if (storage_capable && features.shaderStorageImageReadWithoutFormat) {
            result |= vk::FormatFeatureFlagBits2::eStorageReadWithoutFormat;
        }
        if (storage_capable && features.shaderStorageImageWriteWithoutFormat) {
            result |= vk::FormatFeatureFlagBits2::eStorageWriteWithoutFormat;
        }
    }

    if (!buffer_features && HasDepthComponent(format) &&
        static_cast<bool>(legacy & vk::FormatFeatureFlagBits::eSampledImage)) {
        result |= vk::FormatFeatureFlagBits2::eSampledImageDepthComparison;
    }
    return result;
}
#endif

vk::FormatProperties3 GetFormatProperties(vk::PhysicalDevice physical, vk::Format format) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
    const vk::FormatProperties properties = physical.getFormatProperties(format);
    const vk::PhysicalDeviceFeatures features = physical.getFeatures();
    return vk::FormatProperties3{
        .linearTilingFeatures =
            ExpandLegacyFormatFeatures(format, properties.linearTilingFeatures, features, false),
        .optimalTilingFeatures =
            ExpandLegacyFormatFeatures(format, properties.optimalTilingFeatures, features, false),
        .bufferFeatures =
            ExpandLegacyFormatFeatures(format, properties.bufferFeatures, features, true),
    };
#else
    vk::FormatProperties3 properties3{};
    vk::FormatProperties2 properties2 = {
        .pNext = &properties3,
    };
    physical.getFormatProperties2(format, &properties2);
    return properties3;
#endif
}
'''
s = replace_once(s, old, new, "format_feature_flags2 compatibility")
write(path, s)


# -----------------------------------------------------------------------------
# Finish the legacy render-pass path that already exists in Instance.
# RenderState needs format/sample metadata so Scheduler can build a compatible
# traditional render pass/framebuffer on the Win7 Vulkan 1.2 path.
# -----------------------------------------------------------------------------
path = "src/video_core/renderer_vulkan/vk_scheduler.h"
s = read(path)
s = replace_once(
    s,
    "struct RenderAttachment {\n"
    "    vk::ImageView image_view;\n"
    "    vk::ImageLayout image_layout;\n"
    "    std::array<u32, 4> clear_value;",
    "struct RenderAttachment {\n"
    "    vk::ImageView image_view;\n"
    "    vk::ImageLayout image_layout;\n"
    "    vk::Format format;\n"
    "    vk::SampleCountFlagBits samples{vk::SampleCountFlagBits::e1};\n"
    "    std::array<u32, 4> clear_value;",
    "RenderAttachment format/sample metadata",
)
write(path, s)

path = "src/video_core/renderer_vulkan/vk_rasterizer.cpp"
s = read(path)
s = replace_once(
    s,
    "        attachment.image_view = *image_view.image_view;\n"
    "        attachment.image_layout = image->backing->state.layout;\n"
    "        attachment.clear_value = clear_value.color.uint32;",
    "        attachment.image_view = *image_view.image_view;\n"
    "        attachment.image_layout = image->backing->state.layout;\n"
    "        attachment.format = image_view.info.format;\n"
    "        attachment.samples =\n"
    "            key.color_samples[cb]\n"
    "                ? LiverpoolToVK::NumSamples(key.color_samples[cb], instance.GetColorSampleCounts())\n"
    "                : vk::SampleCountFlagBits::e1;\n"
    "        attachment.clear_value = clear_value.color.uint32;",
    "Color attachment metadata",
)
s = replace_once(
    s,
    "        attachment.image_view = *image_view.image_view;\n"
    "        attachment.image_layout = image.backing->state.layout;\n"
    "        attachment.clear_value = {};",
    "        attachment.image_view = *image_view.image_view;\n"
    "        attachment.image_layout = image.backing->state.layout;\n"
    "        attachment.format = image_view.info.format;\n"
    "        attachment.samples =\n"
    "            key.depth_samples\n"
    "                ? LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts())\n"
    "                : vk::SampleCountFlagBits::e1;\n"
    "        attachment.clear_value = {};",
    "Depth attachment metadata",
)
write(path, s)

path = "src/video_core/renderer_vulkan/vk_scheduler.cpp"
s = read(path)
old = '''void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    current_cmdbuf.beginRendering(rendering_info);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    current_cmdbuf.endRendering();
}
'''
new = '''void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

#ifdef SHADPS4_WINDOWS_7_COMPAT
    LegacyRenderPassKey legacy_key{};
    legacy_key.color_count = render_state.num_color_attachments;

    std::array<vk::ImageView, 9> attachment_views{};
    std::array<vk::ClearAttachment, 9> clear_attachments{};
    u32 attachment_count = 0;
    u32 clear_count = 0;

    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        if (!cb.image_view) {
            continue;
        }
        legacy_key.color_formats[i] = cb.format;
        legacy_key.color_samples[i] = cb.samples;
        legacy_key.color_layouts[i] = cb.image_layout;
        attachment_views[attachment_count++] = cb.image_view;
        if (cb.is_clear) {
            clear_attachments[clear_count++] = vk::ClearAttachment{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .colorAttachment = i,
                .clearValue = vk::ClearValue{
                    .color = vk::ClearColorValue{.uint32 = cb.clear_value}},
            };
        }
    }

    const auto& db = render_state.depth_stencil_attachment;
    legacy_key.has_depth = db.has_depth;
    legacy_key.has_stencil = db.has_stencil;
    if (db.has_depth || db.has_stencil) {
        legacy_key.depth_format = db.format;
        legacy_key.depth_samples = db.samples;
        legacy_key.depth_layout = db.image_layout;
        attachment_views[attachment_count++] = db.image_view;

        vk::ImageAspectFlags clear_aspects{};
        if (db.depth_clear) {
            clear_aspects |= vk::ImageAspectFlagBits::eDepth;
        }
        if (db.stencil_clear) {
            clear_aspects |= vk::ImageAspectFlagBits::eStencil;
        }
        if (clear_aspects) {
            clear_attachments[clear_count++] = vk::ClearAttachment{
                .aspectMask = clear_aspects,
                .colorAttachment = 0,
                .clearValue = vk::ClearValue{
                    .depthStencil = vk::ClearDepthStencilValue{
                        .depth = std::bit_cast<float>(db.clear_value[0]),
                        .stencil = db.clear_value[1],
                    }},
            };
        }
    }

    const LegacyRenderTarget target = instance.GetLegacyRenderTarget(
        legacy_key, render_state.width, render_state.height, render_state.num_layers);
    const vk::RenderPassAttachmentBeginInfo attachment_begin = {
        .attachmentCount = attachment_count,
        .pAttachments = attachment_views.data(),
    };
    const vk::RenderPassBeginInfo begin_info = {
        .pNext = &attachment_begin,
        .renderPass = target.render_pass,
        .framebuffer = target.framebuffer,
        .renderArea = {.offset = {0, 0}, .extent = {render_state.width, render_state.height}},
    };
    current_cmdbuf.beginRenderPass(begin_info, vk::SubpassContents::eInline);

    // Cached compatibility render passes intentionally use LOAD. Reproduce dynamic-rendering
    // CLEAR semantics explicitly so the same render pass stays reusable for clear/non-clear draws.
    if (clear_count != 0) {
        const vk::ClearRect clear_rect = {
            .rect = {.offset = {0, 0}, .extent = {render_state.width, render_state.height}},
            .baseArrayLayer = 0,
            .layerCount = render_state.num_layers,
        };
        current_cmdbuf.clearAttachments(clear_count, clear_attachments.data(), 1, &clear_rect);
    }
#else
    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    current_cmdbuf.beginRendering(rendering_info);
#endif
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
#ifdef SHADPS4_WINDOWS_7_COMPAT
    current_cmdbuf.endRenderPass();
#else
    current_cmdbuf.endRendering();
#endif
}
'''
s = replace_once(s, old, new, "Scheduler legacy render-pass wiring")
write(path, s)


# Graphics pipelines need a compatible traditional render pass on Win7 instead of
# VkPipelineRenderingCreateInfo.
path = "src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp"
s = read(path)
s = replace_once(
    s,
    '#include "common/assert.h"\n',
    '#include "common/assert.h"\n#include "core/emulator_settings.h"\n',
    "graphics pipeline dumb GPU include",
)
s = replace_once(
    s,
    "    BuildDescSetLayout(preloading);\n    const auto debug_str = GetDebugString();",
    "    BuildDescSetLayout(preloading);\n"
    "    if (!desc_layout && EmulatorSettings.IsDumbGPU()) {\n"
    "        LOG_WARNING(Render_Vulkan, \"Dumb GPU: skipping graphics pipeline with invalid descriptor layout\");\n"
    "        return;\n"
    "    }\n"
    "    const auto debug_str = GetDebugString();",
    "graphics desc-layout early skip",
)
s = replace_once(
    s,
    "    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);\n"
    "    ASSERT_MSG(layout_result == vk::Result::eSuccess,\n"
    "               \"Failed to create graphics pipeline layout: {}\", vk::to_string(layout_result));\n"
    "    pipeline_layout = std::move(layout);",
    "    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);\n"
    "    if (layout_result != vk::Result::eSuccess && EmulatorSettings.IsDumbGPU()) {\n"
    "        LOG_WARNING(Render_Vulkan, \"Dumb GPU: skipping graphics pipeline; layout creation failed: {}\",\n"
    "                    vk::to_string(layout_result));\n"
    "        return;\n"
    "    }\n"
    "    ASSERT_MSG(layout_result == vk::Result::eSuccess,\n"
    "               \"Failed to create graphics pipeline layout: {}\", vk::to_string(layout_result));\n"
    "    pipeline_layout = std::move(layout);",
    "graphics pipeline-layout skip",
)
s = replace_once(
    s,
    "    const vk::GraphicsPipelineCreateInfo pipeline_info = {\n"
    "        .pNext = &pipeline_rendering_ci,",
    "    vk::GraphicsPipelineCreateInfo pipeline_info = {\n"
    "        .pNext = &pipeline_rendering_ci,",
    "mutable graphics pipeline info",
)
s = replace_once(
    s,
    "        .pDynamicState = &dynamic_info,\n"
    "        .layout = *pipeline_layout,\n"
    "    };\n\n"
    "    auto [pipeline_result, pipe] =",
    "        .pDynamicState = &dynamic_info,\n"
    "        .layout = *pipeline_layout,\n"
    "    };\n\n"
    "#ifdef SHADPS4_WINDOWS_7_COMPAT\n"
    "    LegacyRenderPassKey legacy_key{};\n"
    "    legacy_key.color_count = key.num_color_attachments;\n"
    "    for (u32 i = 0; i < key.num_color_attachments; ++i) {\n"
    "        legacy_key.color_formats[i] = color_formats[i];\n"
    "        legacy_key.color_samples[i] = color_samples[i];\n"
    "        legacy_key.color_layouts[i] = vk::ImageLayout::eColorAttachmentOptimal;\n"
    "    }\n"
    "    legacy_key.has_depth = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid;\n"
    "    legacy_key.has_stencil = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid;\n"
    "    if (legacy_key.has_depth || legacy_key.has_stencil) {\n"
    "        legacy_key.depth_format = depth_format;\n"
    "        legacy_key.depth_samples =\n"
    "            key.depth_samples\n"
    "                ? LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts())\n"
    "                : vk::SampleCountFlagBits::e1;\n"
    "        legacy_key.depth_layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;\n"
    "    }\n"
    "    // VkAttachmentSampleCountInfoAMD is also valid on the traditional-render-pass path.\n"
    "    pipeline_info.pNext = instance.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr;\n"
    "    pipeline_info.renderPass = instance.GetLegacyRenderPass(legacy_key);\n"
    "    pipeline_info.subpass = 0;\n"
    "#endif\n\n"
    "    auto [pipeline_result, pipe] =",
    "legacy graphics pipeline render pass",
)
s = replace_once(
    s,
    "    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, \"Failed to create graphics pipeline: {}\",\n"
    "               vk::to_string(pipeline_result));\n"
    "    pipeline = std::move(pipe);",
    "    if (pipeline_result != vk::Result::eSuccess && EmulatorSettings.IsDumbGPU()) {\n"
    "        LOG_WARNING(Render_Vulkan, \"Dumb GPU: dropping graphics pipeline {}: {}\", debug_str,\n"
    "                    vk::to_string(pipeline_result));\n"
    "        return;\n"
    "    }\n"
    "    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, \"Failed to create graphics pipeline: {}\",\n"
    "               vk::to_string(pipeline_result));\n"
    "    pipeline = std::move(pipe);",
    "graphics pipeline creation skip",
)
s = replace_once(
    s,
    "    auto [layout_result, layout] =\n"
    "        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);\n"
    "    ASSERT_MSG(layout_result == vk::Result::eSuccess,\n"
    "               \"Failed to create graphics descriptor set layout: {}\", vk::to_string(layout_result));\n"
    "    desc_layout = std::move(layout);",
    "    auto [layout_result, layout] =\n"
    "        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);\n"
    "    if (layout_result != vk::Result::eSuccess && EmulatorSettings.IsDumbGPU()) {\n"
    "        LOG_WARNING(Render_Vulkan,\n"
    "                    \"Dumb GPU: graphics descriptor set layout creation failed: {}\",\n"
    "                    vk::to_string(layout_result));\n"
    "        return;\n"
    "    }\n"
    "    ASSERT_MSG(layout_result == vk::Result::eSuccess,\n"
    "               \"Failed to create graphics descriptor set layout: {}\", vk::to_string(layout_result));\n"
    "    desc_layout = std::move(layout);",
    "graphics descriptor layout skip",
)
write(path, s)


# Pipeline validity lets the cache turn a failed best-effort pipeline into a skipped draw/dispatch.
path = "src/video_core/renderer_vulkan/vk_pipeline_common.h"
s = read(path)
s = replace_once(
    s,
    "    vk::Pipeline Handle() const noexcept {\n"
    "        return *pipeline;\n"
    "    }\n",
    "    vk::Pipeline Handle() const noexcept {\n"
    "        return *pipeline;\n"
    "    }\n\n"
    "    bool IsValid() const noexcept {\n"
    "        return static_cast<bool>(pipeline);\n"
    "    }\n",
    "Pipeline::IsValid",
)
write(path, s)

path = "src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
s = read(path)
s = replace_once(
    s,
    "    return it->second.get();\n}\n\nconst ComputePipeline* PipelineCache::GetComputePipeline()",
    "    return it->second->IsValid() ? it->second.get() : nullptr;\n}\n\nconst ComputePipeline* PipelineCache::GetComputePipeline()",
    "graphics invalid pipeline skip",
)
s = replace_once(
    s,
    "    return it->second.get();\n}\n\nbool PipelineCache::RefreshGraphicsKey()",
    "    return it->second->IsValid() ? it->second.get() : nullptr;\n}\n\nbool PipelineCache::RefreshGraphicsKey()",
    "compute invalid pipeline skip",
)
write(path, s)

path = "src/video_core/renderer_vulkan/vk_compute_pipeline.cpp"
s = read(path)
s = replace_once(
    s,
    '#include "shader_recompiler/info.h"\n',
    '#include "core/emulator_settings.h"\n#include "shader_recompiler/info.h"\n',
    "compute dumb GPU include",
)
s = replace_once(
    s,
    "    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,\n"
    "               \"Failed to create compute descriptor set layout: {}\",\n"
    "               vk::to_string(descriptor_set_result));\n"
    "    desc_layout = std::move(descriptor_set);",
    "    if (descriptor_set_result != vk::Result::eSuccess && EmulatorSettings.IsDumbGPU()) {\n"
    "        LOG_WARNING(Render_Vulkan, \"Dumb GPU: skipping compute pipeline; descriptor layout failed: {}\",\n"
    "                    vk::to_string(descriptor_set_result));\n"
    "        return;\n"
    "    }\n"
    "    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,\n"
    "               \"Failed to create compute descriptor set layout: {}\",\n"
    "               vk::to_string(descriptor_set_result));\n"
    "    desc_layout = std::move(descriptor_set);",
    "compute descriptor layout skip",
)
s = replace_once(
    s,
    "    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);\n"
    "    ASSERT_MSG(layout_result == vk::Result::eSuccess,\n"
    "               \"Failed to create compute pipeline layout: {}\", vk::to_string(layout_result));\n"
    "    pipeline_layout = std::move(layout);",
    "    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);\n"
    "    if (layout_result != vk::Result::eSuccess && EmulatorSettings.IsDumbGPU()) {\n"
    "        LOG_WARNING(Render_Vulkan, \"Dumb GPU: skipping compute pipeline; pipeline layout failed: {}\",\n"
    "                    vk::to_string(layout_result));\n"
    "        return;\n"
    "    }\n"
    "    ASSERT_MSG(layout_result == vk::Result::eSuccess,\n"
    "               \"Failed to create compute pipeline layout: {}\", vk::to_string(layout_result));\n"
    "    pipeline_layout = std::move(layout);",
    "compute pipeline layout skip",
)
s = replace_once(
    s,
    "    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, \"Failed to create compute pipeline: {}\",\n"
    "               vk::to_string(pipeline_result));\n"
    "    pipeline = std::move(pipe);",
    "    if (pipeline_result != vk::Result::eSuccess && EmulatorSettings.IsDumbGPU()) {\n"
    "        LOG_WARNING(Render_Vulkan, \"Dumb GPU: dropping compute pipeline {}: {}\", debug_str,\n"
    "                    vk::to_string(pipeline_result));\n"
    "        return;\n"
    "    }\n"
    "    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, \"Failed to create compute pipeline: {}\",\n"
    "               vk::to_string(pipeline_result));\n"
    "    pipeline = std::move(pipe);",
    "compute pipeline creation skip",
)
write(path, s)


# -----------------------------------------------------------------------------
# Dedicated build workflow for this branch. Keep the original V1 workflow untouched.
# -----------------------------------------------------------------------------
source_workflow = read(".github/workflows/build-win7-v1-compat-only.yml")
build = source_workflow
build = replace_once(build, "name: Build shadPS4 Win7 V1 settings override",
                     "name: Build shadPS4 Win7 Dumb GPU", "build workflow name")
build = replace_once(build, "branches: [win7-v1-compat-only]",
                     "branches: [win7-v1-dumb-gpu]", "build workflow branch")
build = replace_once(build, "Build Windows 7 x64 settings-override target",
                     "Build Windows 7 x64 dumb-GPU target", "build job name")
build = replace_once(build, "Configure Windows 7 settings-override build",
                     "Configure Windows 7 dumb-GPU build", "build configure step")
build = replace_once(build, "Upload Windows 7 V1 settings-override artifact",
                     "Upload Windows 7 V1 dumb-GPU artifact", "build upload step")
build = replace_once(build, "shadps4-win7-v1-settings-override-${{ github.sha }}",
                     "shadps4-win7-v1-dumb-gpu-${{ github.sha }}", "artifact name")
build = replace_once(build, "phase=windows7-compatibility-only-v1",
                     "phase=windows7-dumb-gpu-v1", "build identity phase")
build = replace_once(build, "single_axis=settings-precedence-nullgpu-default-false",
                     "single_axis=vulkan12-fallbacks-and-best-effort-dumb-gpu", "build identity axis")
build = replace_once(build, "vulkan_compat_scope=presenter-only",
                     "vulkan_compat_scope=guest-renderer-best-effort", "build identity scope")
build = replace_once(build, "vulkan_guest_compat_patches=false",
                     "vulkan_guest_compat_patches=true", "build identity compat")
write(".github/workflows/build-win7-v1-dumb-gpu.yml", build)

print("Win7 dumb GPU phase 1 patch applied successfully.")
