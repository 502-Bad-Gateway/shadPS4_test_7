// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <thread>

#include "common/logging/log.h"
#include "common/types.h"

// Include vulkan-hpp header
#define VK_ENABLE_BETA_EXTENSIONS
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_STRUCT_SETTERS
#define VULKAN_HPP_HAS_SPACESHIP_OPERATOR
#define VULKAN_HPP_NO_EXCEPTIONS
// Define assert-on-result to nothing to instead return the result for our handling.
#define VULKAN_HPP_ASSERT_ON_RESULT

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-value"
#include <vulkan/vulkan.hpp>
#pragma clang diagnostic pop

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

// Vulkan 1.2 exposes synchronization2 and the first two extended-dynamic-state revisions through
// extension entry points. Keep the renderer call sites shared while using those entry points in
// the Windows 7 compatibility build.
#ifdef SHADPS4_WINDOWS_7_COMPAT
#define bindVertexBuffers2 bindVertexBuffers2EXT
#define pipelineBarrier2 pipelineBarrier2KHR
#define setCullMode setCullModeEXT
#define setDepthBiasEnable setDepthBiasEnableEXT
#define setDepthBoundsTestEnable setDepthBoundsTestEnableEXT
#define setDepthCompareOp setDepthCompareOpEXT
#define setDepthTestEnable setDepthTestEnableEXT
#define setDepthWriteEnable setDepthWriteEnableEXT
#define setFrontFace setFrontFaceEXT
#define setPrimitiveRestartEnable setPrimitiveRestartEnableEXT
#define setRasterizerDiscardEnable setRasterizerDiscardEnableEXT
#define setScissorWithCount setScissorWithCountEXT
#define setStencilOp setStencilOpEXT
#define setStencilTestEnable setStencilTestEnableEXT
#define setViewportWithCount setViewportWithCountEXT

namespace Vulkan::Win7Forensics {

inline std::atomic<u64> event_sequence{};
inline std::atomic<bool> scan_announced{};
inline const auto scan_epoch = std::chrono::steady_clock::now();

inline u64 TimestampUs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - scan_epoch)
                                .count());
}

inline size_t ThreadToken() {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

inline void AnnounceScanOnce() {
    bool expected = false;
    if (!scan_announced.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "shadps4_scan forensic trace active: Vulkan call ordering, result, timestamp and "
             "thread token capture enabled");
    static constexpr std::array env_names = {
        "VK_LAYER_PATH", "VK_INSTANCE_LAYERS", "VK_ICD_FILENAMES", "VK_DRIVER_FILES",
        "VK_LOADER_DEBUG", "VK_ADD_DRIVER_FILES", "VK_LOADER_LAYERS_ENABLE",
        "VK_LOADER_LAYERS_DISABLE",
    };
    for (const char* name : env_names) {
        if (const char* value = std::getenv(name); value != nullptr) {
            LOG_INFO(Render_Vulkan, "shadps4_scan env {}={}", name, value);
        } else {
            LOG_INFO(Render_Vulkan, "shadps4_scan env {}=<unset>", name);
        }
    }
}

inline u64 Begin(std::string_view operation, std::string_view detail = {}) {
    AnnounceScanOnce();
    const u64 event = event_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG_INFO(Render_Vulkan,
             "Win7 Vulkan forensics BEGIN event={} timestamp_us={} thread={} operation={} detail={}",
             event, TimestampUs(), ThreadToken(), operation, detail);
    return event;
}

inline void End(const u64 event, std::string_view operation, std::string_view result = {}) {
    LOG_INFO(Render_Vulkan,
             "Win7 Vulkan forensics END event={} timestamp_us={} thread={} operation={} result={}",
             event, TimestampUs(), ThreadToken(), operation, result);
}

inline void Checkpoint(std::string_view operation, std::string_view detail = {}) {
    AnnounceScanOnce();
    LOG_INFO(Render_Vulkan,
             "Win7 Vulkan forensics CHECKPOINT timestamp_us={} thread={} operation={} detail={}",
             TimestampUs(), ThreadToken(), operation, detail);
}

} // namespace Vulkan::Win7Forensics
#endif
