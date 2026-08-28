$ErrorActionPreference = 'Stop'

$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Normalize([string]$Text) {
    return $Text.Replace("`r`n", "`n")
}

function Replace-Exact([string]$Path, [string]$Old, [string]$New) {
    $Text = Normalize([System.IO.File]::ReadAllText($Path))
    $Old = Normalize($Old)
    $New = Normalize($New)
    if (-not $Text.Contains($Old)) {
        throw "Build 11 source transform failed: expected text not found in $Path"
    }
    $Text = $Text.Replace($Old, $New)
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
}

function Replace-First([string]$Path, [string]$Old, [string]$New) {
    $Text = Normalize([System.IO.File]::ReadAllText($Path))
    $Old = Normalize($Old)
    $New = Normalize($New)
    $Index = $Text.IndexOf($Old)
    if ($Index -lt 0) {
        throw "Build 11 source transform failed: expected first occurrence not found in $Path"
    }
    $Text = $Text.Substring(0, $Index) + $New + $Text.Substring($Index + $Old.Length)
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
}

# ---- Game serial -> SafeGPU profile -----------------------------------------
$Path = 'src/emulator.cpp'
Replace-Exact $Path @'
    EmulatorSettings.Load(id);
#ifdef SHADPS4_WINDOWS_7_COMPAT_ONLY
'@ @'
    EmulatorSettings.Load(id);
    VideoCore::SafeGpuGate::SetGameSerial(id);
#ifdef SHADPS4_WINDOWS_7_COMPAT_ONLY
'@

Replace-Exact $Path @'
    LOG_INFO(Config, "SafeGPU policy/version: {}", VideoCore::SafeGpuGate::PolicyVersion());
    LOG_INFO(Config, "GPU effective mode: {}", VideoCore::SafeGpuGate::GetEffectiveModeName());
'@ @'
    LOG_INFO(Config, "SafeGPU policy/version: {}", VideoCore::SafeGpuGate::PolicyVersion());
    LOG_INFO(Config, "SafeGPU per-title profile: {}", VideoCore::SafeGpuGate::GetProfileName());
    LOG_INFO(Config, "GPU effective mode: {}", VideoCore::SafeGpuGate::GetEffectiveModeName());
'@

# ---- SafeGPU public policy ---------------------------------------------------
$Path = 'src/video_core/safe_gpu/safe_gpu.h'
Replace-Exact $Path @'
enum class EffectiveGpuMode {
    FullGPU,
    SafeGPU,
    NullGPU,
};
'@ @'
enum class EffectiveGpuMode {
    FullGPU,
    SafeGPU,
    NullGPU,
};

enum class SafeGpuProfile {
    Generic,
    Driveclub,
    Bloodborne,
    Doax3,
    Wipeout,
    WeAreDoomed,
    SonicManiaPlus,
};
'@

Replace-Exact $Path 'return "milestone-2-depthless-color-flat-v1";' 'return "milestone-3-per-title-quarantine-v1";'

Replace-Exact $Path @'
    static EffectiveGpuMode GetEffectiveMode() noexcept;
'@ @'
    static void SetGameSerial(std::string_view game_serial) noexcept;
    static SafeGpuProfile GetProfile() noexcept;
    static std::string_view GetProfileName() noexcept;
    static EffectiveGpuMode GetEffectiveMode() noexcept;
'@

Replace-Exact $Path @'
    static bool ShouldAllowGraphicsPipelineHash(std::uint64_t pipeline_hash) noexcept;
    static bool IsKnownControlGraphicsPipelineHash(std::uint64_t pipeline_hash) noexcept;
    static bool ShouldUseFlatFragment(std::uint64_t pipeline_hash) noexcept;
'@ @'
    static bool ShouldAllowGraphicsPipelineHash(std::uint64_t pipeline_hash) noexcept;
    static bool IsQuarantinedGraphicsPipelineHash(std::uint64_t pipeline_hash) noexcept;
    static bool IsKnownControlGraphicsPipelineHash(std::uint64_t pipeline_hash) noexcept;
    static bool IsDriveclubNativeSafeGraphicsPipelineHash(std::uint64_t pipeline_hash) noexcept;
    static bool ShouldUseFlatFragment(std::uint64_t pipeline_hash) noexcept;
'@

# ---- SafeGPU per-title implementation ---------------------------------------
$Path = 'src/video_core/safe_gpu/safe_gpu.cpp'
Replace-Exact $Path @'
#include "video_core/safe_gpu/safe_gpu.h"

#include <limits>
'@ @'
#include "video_core/safe_gpu/safe_gpu.h"

#include <algorithm>
#include <array>
#include <limits>
'@

$ProfileData = @'
namespace {

SafeGpuProfile active_profile = SafeGpuProfile::Generic;

// Exact Driveclub pipeline hashes that already ran without a crash with native fragment shaders:
// 7 from Build 05 (no sampled resources) + 103 from Build 07 (sampled resources).
constexpr std::array<std::uint64_t, 110> DriveclubNativeSafeHashes = {
    0x01ee1675ff98241bULL,
    0x04136db3c14e5e99ULL,
    0x04be5d3c88bf1823ULL,
    0x0536b154707bf875ULL,
    0x05659ee54d3ff65aULL,
    0x07f70768fa1d4d03ULL,
    0x0cfb8bf6aa2941b7ULL,
    0x13fdd5408fab61e1ULL,
    0x15da64f182e2e285ULL,
    0x16d9597586ab23c0ULL,
    0x1855dbe762fc96e0ULL,
    0x1ac8fe2000be0340ULL,
    0x1b09090d373ed7c2ULL,
    0x1d33d01dad1715caULL,
    0x1d7fb7f1bcdbc360ULL,
    0x1d94bb7d2f2f19aeULL,
    0x1da744697ffd1b12ULL,
    0x1e78689621683f7eULL,
    0x2077d740e5a734acULL,
    0x211e377c20e86a18ULL,
    0x21d75e9828c72c07ULL,
    0x24d1575a8b1583f0ULL,
    0x26a245013c2d64fbULL,
    0x2e18b9209e8b726cULL,
    0x2fec51f60134f37aULL,
    0x3387abc5f9c99216ULL,
    0x339cd238874d7af8ULL,
    0x33e7cbab8b6b65bfULL,
    0x34ba728eb11d8f26ULL,
    0x3713f3ca08f697c7ULL,
    0x381663b339472d99ULL,
    0x38ea5deb1423bab6ULL,
    0x3986f2fa29a251aeULL,
    0x3d686cae3b4c2fefULL,
    0x4142a6d253ac15b3ULL,
    0x421d118af6d96e12ULL,
    0x42d34bbca2b2c4f6ULL,
    0x42f24fe89b393c13ULL,
    0x435d59759e21de5eULL,
    0x44af117af1652730ULL,
    0x49e944a98ec8893bULL,
    0x4caf611e48d4cedfULL,
    0x4d4fda33a5b94df3ULL,
    0x533d13bad2f62a81ULL,
    0x5551c22580d53124ULL,
    0x582cc9f974d5fad1ULL,
    0x597502cb33e4e9f0ULL,
    0x59d5b78636dd73c3ULL,
    0x5cdf512d60378e44ULL,
    0x5d492442ed83a263ULL,
    0x5d7421f80426c8e2ULL,
    0x5dc2fdfb91b43d0eULL,
    0x5de521b0f15423f0ULL,
    0x5e49054ed2e34409ULL,
    0x5edb1a6037d407a3ULL,
    0x5f9af44262295138ULL,
    0x60f5139b3bdbede2ULL,
    0x6cb3dcce6d3c14d7ULL,
    0x780b5df3bb75dd17ULL,
    0x79938d8decd0d3adULL,
    0x7c4732f4a1b0f79fULL,
    0x827bb0e7bb06a886ULL,
    0x85fe8c0bcb4402dfULL,
    0x88c264a73b6fce3dULL,
    0x9236e33fc7eb2076ULL,
    0x9400829bfcf3ad25ULL,
    0x97d6163c868da929ULL,
    0x98b0078394a65bd2ULL,
    0x9c72d2b66a9d7db6ULL,
    0x9ea6c220467f1a4dULL,
    0x9f40a8a1bfa7ce1eULL,
    0xa24822ac8c975671ULL,
    0xa2edd036fafec266ULL,
    0xa313391f69d586c6ULL,
    0xa47c6caaed9302d4ULL,
    0xa7ee3e11ad914c59ULL,
    0xad4a21316873adf2ULL,
    0xaef2401121a0e0bdULL,
    0xb12f3e6e1c67eac0ULL,
    0xb37cd83be0abe263ULL,
    0xb3dc06d2dd4b0680ULL,
    0xb5bde33a1b3c1a52ULL,
    0xb6c21707da3941a2ULL,
    0xb97a68e612f7219fULL,
    0xbdfbde9377091205ULL,
    0xbf847b5565755392ULL,
    0xc7520c55f3a309c9ULL,
    0xca30dbd8408b496dULL,
    0xcb269ea5279fe797ULL,
    0xcb294bcb1b256c85ULL,
    0xcbd9e605ba011568ULL,
    0xcccf857fabbe7013ULL,
    0xd286b2a0fb4d0640ULL,
    0xd455c8ed5c1afe36ULL,
    0xd74cce4ffe6a42a4ULL,
    0xd9a29451db8596efULL,
    0xdab640ed0fe0abf5ULL,
    0xdefa3c0c62d2a8e1ULL,
    0xe8fddb1d2ce0c9deULL,
    0xe9265b803a1bb02bULL,
    0xec545503545200e0ULL,
    0xedbae1a3305ce6deULL,
    0xf0b6fc80991c10f1ULL,
    0xf17dc776becd9b9aULL,
    0xf2f38bde7ac597c4ULL,
    0xf3ec3cf0c0f2bd0aULL,
    0xf7406ce0f0aaaa2aULL,
    0xfb7a82cc2505b913ULL,
    0xfcc688aef6ef1677ULL,
    0xfe1df9526632dc44ULL,
};

static_assert(std::ranges::is_sorted(DriveclubNativeSafeHashes));
'@
Replace-First $Path "namespace {`n" $ProfileData

$ProfileMethods = @'
void SafeGpuGate::SetGameSerial(const std::string_view game_serial) noexcept {
    if (game_serial == "CUSA00003") {
        active_profile = SafeGpuProfile::Driveclub;
    } else if (game_serial == "CUSA03173") {
        active_profile = SafeGpuProfile::Bloodborne;
    } else if (game_serial == "CUSA04555") {
        active_profile = SafeGpuProfile::Doax3;
    } else if (game_serial == "CUSA05670") {
        active_profile = SafeGpuProfile::Wipeout;
    } else if (game_serial == "CUSA02394") {
        active_profile = SafeGpuProfile::WeAreDoomed;
    } else if (game_serial == "CUSA07010") {
        active_profile = SafeGpuProfile::SonicManiaPlus;
    } else {
        active_profile = SafeGpuProfile::Generic;
    }
}

SafeGpuProfile SafeGpuGate::GetProfile() noexcept {
    return active_profile;
}

std::string_view SafeGpuGate::GetProfileName() noexcept {
    switch (active_profile) {
    case SafeGpuProfile::Driveclub:
        return "Driveclub/CUSA00003";
    case SafeGpuProfile::Bloodborne:
        return "Bloodborne/CUSA03173";
    case SafeGpuProfile::Doax3:
        return "DOAX3/CUSA04555";
    case SafeGpuProfile::Wipeout:
        return "Wipeout/CUSA05670";
    case SafeGpuProfile::WeAreDoomed:
        return "WeAreDoomed/CUSA02394";
    case SafeGpuProfile::SonicManiaPlus:
        return "SonicManiaPlus/CUSA07010";
    case SafeGpuProfile::Generic:
    default:
        return "Generic";
    }
}

'@
Replace-Exact $Path 'EffectiveGpuMode SafeGpuGate::GetEffectiveMode() noexcept {' ($ProfileMethods + 'EffectiveGpuMode SafeGpuGate::GetEffectiveMode() noexcept {')

Replace-Exact $Path @'
    return mode == EffectiveGpuMode::SafeGPU && pipeline_hash != 0;
}

bool SafeGpuGate::IsKnownControlGraphicsPipelineHash(
'@ @'
    return mode == EffectiveGpuMode::SafeGPU && pipeline_hash != 0 &&
           !IsQuarantinedGraphicsPipelineHash(pipeline_hash);
}

bool SafeGpuGate::IsQuarantinedGraphicsPipelineHash(
    const std::uint64_t pipeline_hash) noexcept {
    switch (active_profile) {
    case SafeGpuProfile::Bloodborne:
        return pipeline_hash == 0x41ce00fd9bac4b92ULL;
    case SafeGpuProfile::Doax3:
        return pipeline_hash == 0xae5a792de45aaf76ULL;
    case SafeGpuProfile::Wipeout:
        return pipeline_hash == 0x2dc86d47c8a5b854ULL;
    default:
        return false;
    }
}

bool SafeGpuGate::IsKnownControlGraphicsPipelineHash(
'@

Replace-Exact $Path @'
bool SafeGpuGate::ShouldUseFlatFragment(const std::uint64_t pipeline_hash) noexcept {
    return GetEffectiveMode() == EffectiveGpuMode::SafeGPU &&
           !IsKnownControlGraphicsPipelineHashImpl(pipeline_hash);
}
'@ @'
bool SafeGpuGate::IsDriveclubNativeSafeGraphicsPipelineHash(
    const std::uint64_t pipeline_hash) noexcept {
    return std::ranges::binary_search(DriveclubNativeSafeHashes, pipeline_hash);
}

bool SafeGpuGate::ShouldUseFlatFragment(const std::uint64_t pipeline_hash) noexcept {
    if (GetEffectiveMode() != EffectiveGpuMode::SafeGPU ||
        IsKnownControlGraphicsPipelineHashImpl(pipeline_hash)) {
        return false;
    }
    if (active_profile == SafeGpuProfile::Driveclub &&
        IsDriveclubNativeSafeGraphicsPipelineHash(pipeline_hash)) {
        return false;
    }
    return true;
}
'@

Replace-Exact $Path @'
    // Build 08: broaden the safe visible-output probe to simple depthless color/composition
    // pipelines. Their guest fragment module is replaced by the inherited constant-color
    // fragment shader, while depth/stencil, tessellation/geometry, storage-image, logic-op,
    // multisample, MRT and compute complexity remain outside the allow-list.
    return !info.has_depth && !info.has_stencil;
'@ @'
    // Build 11 per-title policy. Generic/Bloodborne/DOAX3/Wipeout retain Build 08/10's
    // depthless-color class. Driveclub additionally admits only the exact native depth pipeline
    // hashes already proven stable in Builds 05 and 07; those hashes keep their guest fragment
    // shader while all other Driveclub candidates remain on the flat-fragment visibility path.
    if (active_profile == SafeGpuProfile::Driveclub &&
        IsDriveclubNativeSafeGraphicsPipelineHash(info.pipeline_hash)) {
        return info.has_depth && !info.has_stencil;
    }
    return !info.has_depth && !info.has_stencil;
'@

# ---- Quarantine before Vulkan pipeline creation -----------------------------
$Path = 'src/video_core/renderer_vulkan/vk_pipeline_cache.cpp'
Replace-Exact $Path @'
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
        if (VideoCore::SafeGpuGate::IsEnabled() &&
            !VideoCore::SafeGpuGate::ShouldAllowGraphicsPipelineHash(pipeline_hash)) {
'@ @'
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
        if (VideoCore::SafeGpuGate::IsEnabled() &&
            VideoCore::SafeGpuGate::IsQuarantinedGraphicsPipelineHash(pipeline_hash)) {
            LOG_WARNING(Render_Vulkan,
                        "[SafeGPU] QUARANTINE profile={} graphics pipeline hash={:#x} before "
                        "vkCreateGraphicsPipelines",
                        VideoCore::SafeGpuGate::GetProfileName(), pipeline_hash);
            infos.fill(nullptr);
            modules.fill(nullptr);
            fetch_shader.reset();
            return nullptr;
        }
        if (VideoCore::SafeGpuGate::IsEnabled() &&
            !VideoCore::SafeGpuGate::ShouldAllowGraphicsPipelineHash(pipeline_hash)) {
'@

Replace-Exact $Path @'
            LOG_INFO(Render_Vulkan,
                     "[SafeGPU] ALLOW graphics pipeline hash={:#x} by Build 06 candidate prefilter "
                     "and geometry-first feature gate before vkCreateGraphicsPipelines",
                     pipeline_hash);
'@ @'
            LOG_INFO(Render_Vulkan,
                     "[SafeGPU] ALLOW profile={} graphics pipeline hash={:#x} by Build 11 "
                     "per-title feature gate before vkCreateGraphicsPipelines",
                     VideoCore::SafeGpuGate::GetProfileName(), pipeline_hash);
'@

# ---- Persistent last-submitted graphics pipeline diagnostic -----------------
$Path = 'src/video_core/renderer_vulkan/vk_rasterizer.h'
Replace-Exact $Path @'
    static bool ShouldLogSafeGpuSample(u64 count) noexcept;
    void LogSafeGpuSummary() const;
    bool IsSafeGpuGraphicsPipeline(const GraphicsPipeline* pipeline) const;
'@ @'
    static bool ShouldLogSafeGpuSample(u64 count) noexcept;
    void LogSafeGpuSummary() const;
    bool IsSafeGpuGraphicsPipeline(const GraphicsPipeline* pipeline) const;
    void PersistLastSubmittedGraphicsPipeline(const GraphicsPipeline* pipeline,
                                              std::string_view draw_type, bool is_indexed);
'@

Replace-Exact $Path @'
    const bool safe_gpu_active;
    SafeGpuStats safe_gpu_stats;
'@ @'
    const bool safe_gpu_active;
    SafeGpuStats safe_gpu_stats;
    u64 safe_gpu_last_submitted_pipeline_hash{};
'@

$Path = 'src/video_core/renderer_vulkan/vk_rasterizer.cpp'
Replace-Exact $Path '#include "common/debug.h"' "#include `"common/debug.h`"`n#include `"common/path_util.h`""
Replace-Exact $Path @'
#include "video_core/texture_cache/texture_cache.h"

#ifdef MemoryBarrier
'@ @'
#include "video_core/texture_cache/texture_cache.h"

#include <filesystem>
#include <fstream>

#ifdef MemoryBarrier
'@

$PersistMethod = @'
void Rasterizer::PersistLastSubmittedGraphicsPipeline(const GraphicsPipeline* pipeline,
                                                      const std::string_view draw_type,
                                                      const bool is_indexed) {
    if (!safe_gpu_active || !pipeline) {
        return;
    }
    const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey());
    if (pipeline_hash == safe_gpu_last_submitted_pipeline_hash) {
        return;
    }
    safe_gpu_last_submitted_pipeline_hash = pipeline_hash;

    const auto directory =
        Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "pipeline_forensics";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        LOG_ERROR(Render_Vulkan,
                  "[SafeGPU] could not create persistent last-pipeline directory: {}",
                  error.message());
        return;
    }

    const auto path = directory / "last_submitted_graphics_pipeline.txt";
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        LOG_ERROR(Render_Vulkan,
                  "[SafeGPU] could not open persistent last-pipeline diagnostic file");
        return;
    }
    output << "format_version=1\n";
    output << "profile=" << VideoCore::SafeGpuGate::GetProfileName() << "\n";
    output << "pipeline_hash=0x" << std::hex << pipeline_hash << std::dec << "\n";
    output << "draw_type=" << draw_type << "\n";
    output << "indexed=" << (is_indexed ? "true" : "false") << "\n";
    output.flush();

    LOG_WARNING(Render_Vulkan,
                "[SafeGPU] LAST_SUBMITTED_GRAPHICS profile={} hash={:#x} draw={} indexed={}",
                VideoCore::SafeGpuGate::GetProfileName(), pipeline_hash, draw_type, is_indexed);
}

'@
Replace-Exact $Path 'bool Rasterizer::IsSafeGpuGraphicsPipeline(const GraphicsPipeline* pipeline) const {' ($PersistMethod + 'bool Rasterizer::IsSafeGpuGraphicsPipeline(const GraphicsPipeline* pipeline) const {')

$GraphicsBind = '    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());'
Replace-First $Path $GraphicsBind ($GraphicsBind + "`n    PersistLastSubmittedGraphicsPipeline(pipeline, `"direct`", is_indexed);")
Replace-First $Path $GraphicsBind ($GraphicsBind + "`n    PersistLastSubmittedGraphicsPipeline(pipeline, `"indirect`", is_indexed);")

Write-Host 'Build 11 source transform completed successfully.'
