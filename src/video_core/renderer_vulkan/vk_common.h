// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
#endif
