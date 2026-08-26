// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

void Pipeline::BindResources(DescriptorWrites& set_writes, const BufferBarriers& buffer_barriers,
                             const Shader::PushData& push_data) const {
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    if (!buffer_barriers.empty()) {
        const auto dependencies = vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = u32(buffer_barriers.size()),
            .pBufferMemoryBarriers = buffer_barriers.data(),
        };
        scheduler.EndRendering();
#ifdef SHADPS4_WINDOWS_7_COMPAT
        const auto forensic_event = Win7Forensics::Begin(
            "cmd_pipeline_barrier2", fmt::format("buffers={}", buffer_barriers.size()));
#endif
        cmdbuf.pipelineBarrier2(dependencies);
#ifdef SHADPS4_WINDOWS_7_COMPAT
        Win7Forensics::End(forensic_event, "cmd_pipeline_barrier2", "recorded");
#endif
    }

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
#ifdef SHADPS4_WINDOWS_7_COMPAT
    const auto push_constant_event = Win7Forensics::Begin("cmd_push_constants");
#endif
    cmdbuf.pushConstants(*pipeline_layout, stage_flags, 0u, sizeof(push_data), &push_data);
#ifdef SHADPS4_WINDOWS_7_COMPAT
    Win7Forensics::End(push_constant_event, "cmd_push_constants", "recorded");
#endif

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
        const auto forensic_event = Win7Forensics::Begin(
            "cmd_push_descriptor_set", fmt::format("writes={}", set_writes.size()));
#endif
        cmdbuf.pushDescriptorSetKHR(bind_point, *pipeline_layout, 0, set_writes);
#ifdef SHADPS4_WINDOWS_7_COMPAT
        Win7Forensics::End(forensic_event, "cmd_push_descriptor_set", "recorded");
#endif
        return;
    }

    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
#ifdef SHADPS4_WINDOWS_7_COMPAT
    const auto forensic_event = Win7Forensics::Begin(
        "cmd_bind_descriptor_sets", fmt::format("writes={}", set_writes.size()));
#endif
    cmdbuf.bindDescriptorSets(bind_point, *pipeline_layout, 0, desc_set, {});
#ifdef SHADPS4_WINDOWS_7_COMPAT
    Win7Forensics::End(forensic_event, "cmd_bind_descriptor_sets", "recorded");
#endif
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
