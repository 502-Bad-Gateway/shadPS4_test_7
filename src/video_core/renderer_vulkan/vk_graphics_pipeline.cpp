// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string_view>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::Backend::SPIRV::AuxShaderType;

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

#ifdef SHADPS4_WINDOWS_7_COMPAT
static std::string DumpPipelineForensicsAuxShader(const GraphicsPipelineForensics& forensics,
                                                  std::span<const u32> code,
                                                  std::string_view name) {
    if (forensics.sequence == 0 || forensics.run_directory.empty() || code.empty()) {
        return {};
    }

    using namespace Common::FS;
    const u64 spv_hash = XXH3_64bits(code.data(), code.size_bytes());
    const auto filename = fmt::format("pipeline_{:06}_{}_spv_{:016x}.spv", forensics.sequence,
                                      name, spv_hash);
    const auto relative_path = std::filesystem::path{"modules"} / filename;
    const auto full_path = forensics.run_directory / relative_path;
    const IOFile file{full_path, FileAccessMode::Create};
    if (file.WriteSpan(code) != code.size()) {
        LOG_ERROR(Render_Vulkan, "Win7 pipeline forensics failed to write auxiliary shader {}",
                  PathToUTF8String(full_path));
        return {};
    }
    return PathToUTF8String(relative_path);
}
#endif

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const vk::ShaderModule> modules, SerializationSupport& sdata, bool preloading,
    const GraphicsPipelineForensics& forensics)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_},
      fetch_shader{std::move(fetch_shader_)}, pipeline_forensics{forensics} {
    const vk::Device device = instance.GetDevice();
    std::ranges::copy(infos, stages.begin());
    BuildDescSetLayout(preloading);
    const auto debug_str = GetDebugString();

    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);

    if (!preloading) {
        VertexInputs<AmdGpu::Buffer> guest_buffers;
        if (!instance.IsVertexInputDynamicState()) {
            const auto& vs_info = runtime_infos[u32(Shader::LogicalStage::Vertex)].vs_info;
            GetVertexInputs(sdata.vertex_attributes, sdata.vertex_bindings, sdata.divisors,
                            guest_buffers, vs_info.step_rate_0, vs_info.step_rate_1);
        }
    }

    const vk::PipelineVertexInputDivisorStateCreateInfo divisor_state = {
        .vertexBindingDivisorCount = static_cast<u32>(sdata.divisors.size()),
        .pVertexBindingDivisors = sdata.divisors.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .pNext = sdata.divisors.empty() ? nullptr : &divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(sdata.vertex_bindings.size()),
        .pVertexBindingDescriptions = sdata.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(sdata.vertex_attributes.size()),
        .pVertexAttributeDescriptions = sdata.vertex_attributes.data(),
    };

    const auto topology = LiverpoolToVK::PrimitiveType(key.prim_type);
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = topology,
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    const vk::PipelineTessellationStateCreateInfo tessellation_state = {
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    vk::StructureChain raster_chain = {
        vk::PipelineRasterizationStateCreateInfo{
            .depthClampEnable = key.depth_clamp_enable &&
                                (!key.depth_clip_enable || instance.IsDepthClipEnableSupported()),
            .rasterizerDiscardEnable = false,
            .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
            .lineWidth = 1.0f,
        },
        vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT{
            .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                       ? vk::ProvokingVertexModeEXT::eFirstVertex
                                       : vk::ProvokingVertexModeEXT::eLastVertex,
        },
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT{
            .depthClipEnable = key.depth_clip_enable,
        },
    };

    if (!instance.IsProvokingVertexSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!instance.IsDepthClipEnableSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    if (!preloading) {
        const auto& fs_info = runtime_infos[u32(Shader::LogicalStage::Fragment)].fs_info;
        sdata.multisampling = {
            .rasterizationSamples = LiverpoolToVK::NumSamples(
                key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
            .sampleShadingEnable =
                fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
        };
    }

    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control = {
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };

    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .pNext = instance.IsDepthClipControlSupported() ? &clip_control : nullptr,
    };

#ifdef SHADPS4_WINDOWS_7_COMPAT
    boost::container::static_vector<vk::DynamicState, 32> dynamic_states = {
        vk::DynamicState::eViewportWithCountEXT,
        vk::DynamicState::eScissorWithCountEXT,
        vk::DynamicState::eBlendConstants,
        vk::DynamicState::eDepthTestEnableEXT,
        vk::DynamicState::eDepthWriteEnableEXT,
        vk::DynamicState::eDepthCompareOpEXT,
        vk::DynamicState::eDepthBiasEnableEXT,
        vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnableEXT,
        vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask,
        vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOpEXT,
        vk::DynamicState::eCullModeEXT,
        vk::DynamicState::eFrontFaceEXT,
        vk::DynamicState::eRasterizerDiscardEnableEXT,
        vk::DynamicState::eLineWidth,
        vk::DynamicState::ePrimitiveRestartEnableEXT,
    };
#else
    boost::container::static_vector<vk::DynamicState, 32> dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };
#endif

    if (instance.IsDepthBoundsSupported()) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnableEXT);
#else
        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
#endif
        dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!sdata.vertex_bindings.empty()) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
        dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStrideEXT);
#else
        dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
#endif
    }

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

#ifdef SHADPS4_WINDOWS_7_COMPAT
    std::string auxiliary_tcs_file;
    std::string auxiliary_tes_file;
    std::string auxiliary_fragment_file;
#endif

    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
        shader_stages;
    auto stage = u32(Shader::LogicalStage::Vertex);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::Geometry);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::TessellationControl);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::LogicalStage::Fragment)].fs_info;
            sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs_info);
        }
#ifdef SHADPS4_WINDOWS_7_COMPAT
        auxiliary_tcs_file =
            DumpPipelineForensicsAuxShader(pipeline_forensics, sdata.tcs, "aux_tcs");
#endif
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = CompileSPV(sdata.tcs, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::TessellationEval);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::LogicalStage::Fragment)].fs_info;
            sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
                AuxShaderType::PassthroughTES, fs_info);
        }
#ifdef SHADPS4_WINDOWS_7_COMPAT
        auxiliary_tes_file =
            DumpPipelineForensicsAuxShader(pipeline_forensics, sdata.tes, "aux_tes");
#endif
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = CompileSPV(sdata.tes, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::Fragment);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (runtime_infos[u32(Shader::LogicalStage::Fragment)].fs_info.clip_distance_emulation) {
        if (!preloading) {
            const auto vs_runtime_info =
                runtime_infos[static_cast<u32>(Shader::LogicalStage::Vertex)].vs_info;

            sdata.fragment =
                Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs_runtime_info.outputs);
        }
#ifdef SHADPS4_WINDOWS_7_COMPAT
        auxiliary_fragment_file = DumpPipelineForensicsAuxShader(
            pipeline_forensics, sdata.fragment, "aux_fragment");
#endif
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = CompileSPV(sdata.fragment, instance.GetDevice()),
            .pName = "main",
        });
    }

    const auto depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats{};
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!instance.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        color_formats[i] = color_format;
    }

    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    std::ranges::transform(key.color_samples, color_samples.begin(), [&instance](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    const vk::AttachmentSampleCountInfoAMD mixed_samples = {
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };

#ifdef SHADPS4_WINDOWS_7_COMPAT
    legacy_render_pass_key.color_count = key.num_color_attachments;
    for (u32 index = 0; index < key.num_color_attachments; ++index) {
        if (key.mrt_mask & (1U << index)) {
            legacy_render_pass_key.color_formats[index] = color_formats[index];
            legacy_render_pass_key.color_samples[index] = color_samples[index];
        }
    }
    legacy_render_pass_key.depth_format = depth_format;
    legacy_render_pass_key.depth_samples =
        LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts());
    legacy_render_pass_key.has_depth = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid;
    legacy_render_pass_key.has_stencil =
        key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid;
    const vk::RenderPass legacy_render_pass = instance.GetLegacyRenderPass(legacy_render_pass_key);
#else
    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .pNext = instance.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? depth_format
                                       : vk::Format::eUndefined,
    };
#endif

    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments;
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne);
        const auto alpha_scaled_min_max =
            (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) &&
            (src_alpha != vk::BlendFactor::eOne || dst_alpha != vk::BlendFactor::eOne);
        if (color_scaled_min_max || alpha_scaled_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = src_alpha,
            .dstAlphaBlendFactor = dst_alpha,
            .alphaBlendOp = alpha_blend,
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it.
    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil_info = {};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
#ifdef SHADPS4_WINDOWS_7_COMPAT
        .pNext = nullptr,
#else
        .pNext = &pipeline_rendering_ci,
#endif
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &vertex_input_info : nullptr,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = &tessellation_state,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_chain.get(),
        .pMultisampleState = &sdata.multisampling,
        .pDepthStencilState =
            !instance.IsExtendedDynamicState3Supported() ? &depth_stencil_info : nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
#ifdef SHADPS4_WINDOWS_7_COMPAT
        .renderPass = legacy_render_pass,
#endif
    };

#ifdef SHADPS4_WINDOWS_7_COMPAT
    std::filesystem::path pipeline_forensics_report_path;
    if (pipeline_forensics.sequence != 0 && !pipeline_forensics.run_directory.empty()) {
        using namespace Common::FS;
        const auto report_stem =
            fmt::format("pipeline_{:06}_{:016x}", pipeline_forensics.sequence,
                        pipeline_forensics.pipeline_hash);
        pipeline_forensics_report_path = pipeline_forensics.run_directory / (report_stem + ".txt");
        const auto key_path = pipeline_forensics.run_directory / (report_stem + ".key.bin");

        std::string report;
        report += "format_version=1\nstatus=started\n";
        report += fmt::format("pipeline_id={}\npipeline_hash=0x{:016x}\npreloading={}\n",
                              pipeline_forensics.sequence, pipeline_forensics.pipeline_hash,
                              preloading);
        report += fmt::format("vulkan_api={}.{}.{}\ndriver_id={}\nsupported_spirv=0x{:08x}\n",
                              VK_VERSION_MAJOR(instance.ApiVersion()),
                              VK_VERSION_MINOR(instance.ApiVersion()),
                              VK_VERSION_PATCH(instance.ApiVersion()),
                              vk::to_string(instance.GetDriverID()), profile.supported_spirv);
        report += fmt::format(
            "pipeline_cache_present={}\npipeline_create_flags=0\nlegacy_render_pass=true\n"
            "stage_count={}\n",
            bool(pipeline_cache), shader_stages.size());

        for (u32 index = 0; index < MaxShaderStages; ++index) {
            if (!infos[index]) {
                continue;
            }
            report += fmt::format(
                "stage[{}].guest_stage={}\nstage[{}].logical_stage={}\n"
                "stage[{}].program_hash=0x{:016x}\nstage[{}].permutation_hash=0x{:016x}\n"
                "stage[{}].spv_file={}\n",
                index, infos[index]->stage, index, index, index, infos[index]->pgm_hash, index,
                static_cast<u64>(key.stage_hashes[index]), index,
                pipeline_forensics.shader_files[index].empty()
                    ? std::string_view{"<not-captured>"}
                    : std::string_view{pipeline_forensics.shader_files[index]});
        }
        for (u32 index = 0; index < shader_stages.size(); ++index) {
            report += fmt::format("pipeline_stage[{}]={}\n", index,
                                  vk::to_string(shader_stages[index].stage));
        }
        if (!auxiliary_tcs_file.empty()) {
            report += fmt::format("aux_tcs_spv_file={}\n", auxiliary_tcs_file);
        }
        if (!auxiliary_tes_file.empty()) {
            report += fmt::format("aux_tes_spv_file={}\n", auxiliary_tes_file);
        }
        if (!auxiliary_fragment_file.empty()) {
            report += fmt::format("aux_fragment_spv_file={}\n", auxiliary_fragment_file);
        }

        report += pipeline_forensics_descriptor_state;
        report += fmt::format(
            "topology={}\nprimitive_type={}\npatch_control_points={}\n"
            "depth_clamp_enable={}\ndepth_clip_enable={}\npolygon_mode={}\n"
            "provoking_vertex_last={}\nvertex_input_dynamic={}\n"
            "extended_dynamic_state3={}\ndynamic_color_write_mask={}\n"
            "mixed_any_samples={}\nmixed_depth_samples={}\n",
            vk::to_string(topology), static_cast<u32>(key.prim_type),
            tessellation_state.patchControlPoints, raster_chain.get().depthClampEnable,
            static_cast<bool>(key.depth_clip_enable), vk::to_string(raster_chain.get().polygonMode),
            static_cast<u32>(key.provoking_vtx_last), instance.IsVertexInputDynamicState(),
            instance.IsExtendedDynamicState3Supported(),
            instance.IsDynamicColorWriteMaskSupported(), instance.IsMixedAnySamplesSupported(),
            instance.IsMixedDepthSamplesSupported());

        report += fmt::format("vertex_attribute_count={}\nvertex_binding_count={}\n"
                              "vertex_divisor_count={}\n",
                              sdata.vertex_attributes.size(), sdata.vertex_bindings.size(),
                              sdata.divisors.size());
        for (u32 index = 0; index < sdata.vertex_attributes.size(); ++index) {
            const auto& attribute = sdata.vertex_attributes[index];
            report += fmt::format(
                "vertex_attribute[{}]=location:{},binding:{},format:{},offset:{}\n", index,
                attribute.location, attribute.binding, vk::to_string(attribute.format),
                attribute.offset);
        }
        for (u32 index = 0; index < sdata.vertex_bindings.size(); ++index) {
            const auto& binding = sdata.vertex_bindings[index];
            report += fmt::format("vertex_binding[{}]=binding:{},stride:{},input_rate:{}\n", index,
                                  binding.binding, binding.stride,
                                  vk::to_string(binding.inputRate));
        }
        for (u32 index = 0; index < sdata.divisors.size(); ++index) {
            const auto& divisor = sdata.divisors[index];
            report += fmt::format("vertex_divisor[{}]=binding:{},divisor:{}\n", index,
                                  divisor.binding, divisor.divisor);
        }

        report += fmt::format(
            "rasterization_samples={}\nsample_shading_enable={}\nmin_sample_shading={}\n"
            "alpha_to_coverage_enable={}\nalpha_to_one_enable={}\n"
            "color_attachment_count={}\nmrt_mask=0x{:x}\n",
            vk::to_string(sdata.multisampling.rasterizationSamples),
            bool(sdata.multisampling.sampleShadingEnable), sdata.multisampling.minSampleShading,
            bool(sdata.multisampling.alphaToCoverageEnable),
            bool(sdata.multisampling.alphaToOneEnable), key.num_color_attachments, key.mrt_mask);
        for (u32 index = 0; index < key.num_color_attachments; ++index) {
            const auto& attachment = attachments[index];
            report += fmt::format(
                "color[{}].active={}\ncolor[{}].format={}\ncolor[{}].samples={}\n"
                "color[{}].write_mask=0x{:x}\ncolor[{}].blend_enable={}\n"
                "color[{}].src_color={}\ncolor[{}].dst_color={}\ncolor[{}].color_op={}\n"
                "color[{}].src_alpha={}\ncolor[{}].dst_alpha={}\ncolor[{}].alpha_op={}\n",
                index, bool(key.mrt_mask & (1U << index)), index,
                vk::to_string(legacy_render_pass_key.color_formats[index]), index,
                vk::to_string(legacy_render_pass_key.color_samples[index]), index,
                static_cast<VkColorComponentFlags>(attachment.colorWriteMask), index,
                bool(attachment.blendEnable), index, vk::to_string(attachment.srcColorBlendFactor),
                index, vk::to_string(attachment.dstColorBlendFactor), index,
                vk::to_string(attachment.colorBlendOp), index,
                vk::to_string(attachment.srcAlphaBlendFactor), index,
                vk::to_string(attachment.dstAlphaBlendFactor), index,
                vk::to_string(attachment.alphaBlendOp));
        }
        report += fmt::format(
            "depth_format={}\ndepth_samples={}\ndepth_layout={}\nhas_depth={}\n"
            "has_stencil={}\ndepth_stencil_state_present={}\nlogic_op_enable={}\nlogic_op={}\n"
            "dynamic_state_count={}\n",
            vk::to_string(legacy_render_pass_key.depth_format),
            vk::to_string(legacy_render_pass_key.depth_samples),
            vk::to_string(legacy_render_pass_key.depth_layout), legacy_render_pass_key.has_depth,
            legacy_render_pass_key.has_stencil, pipeline_info.pDepthStencilState != nullptr,
            bool(color_blending.logicOpEnable), vk::to_string(color_blending.logicOp),
            dynamic_states.size());
        for (u32 index = 0; index < dynamic_states.size(); ++index) {
            report += fmt::format("dynamic_state[{}]={}\n", index,
                                  vk::to_string(dynamic_states[index]));
        }
        report += fmt::format("raw_pipeline_key_file={}\n",
                              PathToUTF8String(key_path.filename()));

        const IOFile key_file{key_path, FileAccessMode::Create};
        if (key_file.IsOpen()) {
            key_file.WriteRaw<u8>(&key, sizeof(key));
        }
        const IOFile report_file{pipeline_forensics_report_path, FileAccessMode::Create,
                                 FileType::TextFile};
        report_file.WriteString(report);
        LOG_WARNING(Render_Vulkan,
                    "Win7 pipeline forensics START id={} hash=0x{:016x} report={}",
                    pipeline_forensics.sequence, pipeline_forensics.pipeline_hash,
                    PathToUTF8String(pipeline_forensics_report_path));
    }
#endif

    auto [pipeline_result, pipe] =
        device.createGraphicsPipelineUnique(pipeline_cache, pipeline_info);
#ifdef SHADPS4_WINDOWS_7_COMPAT
    if (!pipeline_forensics_report_path.empty()) {
        using namespace Common::FS;
        const bool succeeded = pipeline_result == vk::Result::eSuccess;
        const auto completion = fmt::format("status={}\nvk_result={}\n",
                                            succeeded ? "success" : "returned_error",
                                            vk::to_string(pipeline_result));
        const IOFile report_file{pipeline_forensics_report_path, FileAccessMode::Append,
                                 FileType::TextFile};
        report_file.WriteString(completion);
        if (succeeded) {
            LOG_WARNING(Render_Vulkan, "Win7 pipeline forensics SUCCESS id={} hash=0x{:016x}",
                        pipeline_forensics.sequence, pipeline_forensics.pipeline_hash);
        } else {
            LOG_ERROR(Render_Vulkan,
                      "Win7 pipeline forensics RETURNED_ERROR id={} hash=0x{:016x} result={}",
                      pipeline_forensics.sequence, pipeline_forensics.pipeline_hash,
                      vk::to_string(pipeline_result));
        }
    }
#endif
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
}

GraphicsPipeline::~GraphicsPipeline() = default;

template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::LogicalStage::Vertex);
    for (const auto& attrib : fetch_shader->attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        attributes.push_back(Attribute{
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = attrib.semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;

void GraphicsPipeline::BuildDescSetLayout(bool preloading) {
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto stage_bit = LogicalStageToStageBit[u32(stage->l_stage)];
        for (const auto& buffer : stage->buffers) {
            const auto sharp =
                preloading ? AmdGpu::Buffer{}
                           : buffer.GetSharp(*stage); // See for the comment in compute PL creation
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
#ifdef SHADPS4_WINDOWS_7_COMPAT
    if (pipeline_forensics.sequence != 0) {
        pipeline_forensics_descriptor_state =
            fmt::format("descriptor_binding_count={}\nuses_push_descriptors={}\n"
                        "max_push_descriptors={}\n",
                        bindings.size(), uses_push_descriptors, instance.MaxPushDescriptors());
        for (u32 index = 0; index < bindings.size(); ++index) {
            const auto& descriptor = bindings[index];
            pipeline_forensics_descriptor_state += fmt::format(
                "descriptor[{}]=binding:{},type:{},count:{},stages:{}\n", index,
                descriptor.binding, vk::to_string(descriptor.descriptorType),
                descriptor.descriptorCount, vk::to_string(descriptor.stageFlags));
        }
    }
#endif
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [layout_result, layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics descriptor set layout: {}", vk::to_string(layout_result));
    desc_layout = std::move(layout);
}

} // namespace Vulkan
