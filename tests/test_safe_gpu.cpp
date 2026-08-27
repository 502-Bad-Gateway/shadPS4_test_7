// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>

#include <gtest/gtest.h>

#include "core/emulator_settings.h"
#include "video_core/safe_gpu/safe_gpu.h"

class SafeGpuPolicyTest : public ::testing::Test {
protected:
    void SetUp() override {
        settings = std::make_shared<EmulatorSettingsImpl>();
        EmulatorSettingsImpl::SetInstance(settings);
    }

    void TearDown() override {
        EmulatorSettingsImpl::SetInstance(nullptr);
        settings.reset();
    }

    std::shared_ptr<EmulatorSettingsImpl> settings;
};

TEST_F(SafeGpuPolicyTest, MilestoneOneModePrecedenceAndBinding) {
    settings->SetNullGPU(false);
    settings->SetSafeGPU(false);
    EXPECT_EQ(VideoCore::SafeGpuGate::GetEffectiveMode(), VideoCore::EffectiveGpuMode::FullGPU);
    EXPECT_TRUE(VideoCore::SafeGpuGate::ShouldBindGuestRasterizer());
    EXPECT_TRUE(VideoCore::SafeGpuGate::ShouldAllowGraphics());
    EXPECT_TRUE(VideoCore::SafeGpuGate::ShouldAllowCompute());

    settings->SetSafeGPU(true);
    EXPECT_EQ(VideoCore::SafeGpuGate::GetEffectiveMode(), VideoCore::EffectiveGpuMode::SafeGPU);
    EXPECT_TRUE(VideoCore::SafeGpuGate::ShouldBindGuestRasterizer());
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldAllowGraphics());
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldAllowCompute());
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldAllowGuestCpSync());
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldWaitForGuestRewind());
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldAllowGdsTransfers());

    settings->SetNullGPU(true);
    EXPECT_EQ(VideoCore::SafeGpuGate::GetEffectiveMode(), VideoCore::EffectiveGpuMode::NullGPU);
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldBindGuestRasterizer());
}

TEST_F(SafeGpuPolicyTest, MilestoneOneTransferAllowListFailsClosed) {
    settings->SetNullGPU(false);
    settings->SetSafeGPU(true);

    EXPECT_TRUE(VideoCore::SafeGpuGate::ShouldAllowSimpleBufferFill(0x1000, 16, false));
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldAllowSimpleBufferFill(0, 16, false));
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldAllowSimpleBufferFill(0x1002, 16, false));
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldAllowSimpleBufferFill(0x1000, 6, false));
    EXPECT_FALSE(VideoCore::SafeGpuGate::ShouldAllowSimpleBufferFill(0x1000, 16, true));

    EXPECT_TRUE(
        VideoCore::SafeGpuGate::ShouldAllowSimpleBufferCopy(0x2000, 0x1000, 16, false, false));
    EXPECT_FALSE(
        VideoCore::SafeGpuGate::ShouldAllowSimpleBufferCopy(0x1008, 0x1000, 16, false, false));
    EXPECT_FALSE(
        VideoCore::SafeGpuGate::ShouldAllowSimpleBufferCopy(0x2000, 0x1000, 16, true, false));
    EXPECT_FALSE(
        VideoCore::SafeGpuGate::ShouldAllowSimpleBufferCopy(0x2000, 0x1000, 0, false, false));
}
