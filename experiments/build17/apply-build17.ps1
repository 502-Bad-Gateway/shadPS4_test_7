$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Normalize([string]$Text) {
    return $Text.Replace("`r`n", "`n")
}

function Replace-Exact([string]$Path, [string]$Old, [string]$New) {
    $Text = Normalize([System.IO.File]::ReadAllText($Path))
    $Old = Normalize($Old)
    $New = Normalize($New)
    $First = $Text.IndexOf($Old)
    if ($First -lt 0 -or $Text.IndexOf($Old, $First + 1) -ge 0) {
        throw "Build 17 transform failed: expected text was not found exactly once in $Path"
    }
    $Text = $Text.Substring(0, $First) + $New + $Text.Substring($First + $Old.Length)
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
}

function Count-Occurrences([string]$Text, [string]$Needle) {
    $Count = 0
    $Index = 0
    while (($Index = $Text.IndexOf($Needle, $Index)) -ge 0) {
        ++$Count
        $Index += $Needle.Length
    }
    return $Count
}

# BUILD 17 is diagnostics-only on top of BUILD 16-r2. Compute remains fail-closed and is still
# returned before guest compute shader/pipeline creation. The only new behavior is persistent
# Driveclub compute workload forensics so the skipped producers can be classified before any
# selective compute family is enabled in a later experiment.

# Runtime policy identity.
$Path = 'src/video_core/safe_gpu/safe_gpu.h'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$OldPolicy = 'return "milestone-8r2-driveclub-exact-fullgpu-prefix-gaps-postcreate-v1";'
$NewPolicy = 'return "milestone-9-driveclub-compute-forensics-v1";'
$Index = $Text.IndexOf($OldPolicy)
if ($Index -lt 0 -or $Text.IndexOf($OldPolicy, $Index + 1) -ge 0) {
    throw 'Build 17 transform failed: BUILD 16-r2 policy identity was not found exactly once'
}
$Text = $Text.Substring(0, $Index) + $NewPolicy + $Text.Substring($Index + $OldPolicy.Length)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Expose only the command-processor context needed by the diagnostic recorder: current queue id
# and the address of the current PM4 DispatchDirect/DispatchIndirect packet.
$Path = 'src/video_core/amdgpu/liverpool.h'
Replace-Exact $Path @'
    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }
'@ @'
    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    [[nodiscard]] s32 GetCurrentQueueId() const noexcept {
        return curr_qid;
    }

    [[nodiscard]] VAddr GetCurrentComputeCommandAddress() const noexcept {
        return current_compute_command_address;
    }
'@

Replace-Exact $Path @'
    VAddr indirect_args_addr{};
'@ @'
    // BUILD 17 diagnostic only: current guest PM4 compute command packet address.
    VAddr current_compute_command_address{};
    VAddr indirect_args_addr{};
'@

# Capture the PM4 packet address at both graphics-queue and async-compute dispatch sites without
# changing dispatch semantics. There are two parser sites for each opcode in this source.
$Path = 'src/video_core/amdgpu/liverpool.cpp'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$Direct = 'case PM4ItOpcode::DispatchDirect: {'
$Indirect = 'case PM4ItOpcode::DispatchIndirect: {'
$DirectCount = Count-Occurrences $Text $Direct
$IndirectCount = Count-Occurrences $Text $Indirect
if ($DirectCount -ne 2 -or $IndirectCount -ne 2) {
    throw "Build 17 transform failed: expected exactly two direct and two indirect PM4 parser sites; found direct=$DirectCount indirect=$IndirectCount"
}
$Text = $Text.Replace($Direct, $Direct + "`n                // BUILD 17 diagnostic only: preserve guest PM4 packet address for compute forensics.`n                current_compute_command_address = reinterpret_cast<VAddr>(header);")
$Text = $Text.Replace($Indirect, $Indirect + "`n                // BUILD 17 diagnostic only: preserve guest PM4 packet address for compute forensics.`n                current_compute_command_address = reinterpret_cast<VAddr>(header);")
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Add compact in-memory aggregation to Rasterizer. The map key is the complete CSV signature;
# counts are periodically flushed so useful data survives the known no-save Driveclub assertion.
$Path = 'src/video_core/renderer_vulkan/vk_rasterizer.h'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$Pragma = "#pragma once`n`n"
$PragmaIndex = $Text.IndexOf($Pragma)
if ($PragmaIndex -lt 0 -or $Text.IndexOf($Pragma, $PragmaIndex + 1) -ge 0) {
    throw 'Build 17 transform failed: vk_rasterizer.h pragma anchor was not found exactly once'
}
$Text = $Text.Substring(0, $PragmaIndex) + "#pragma once`n`n#include <map>`n#include <string>`n`n" + $Text.Substring($PragmaIndex + $Pragma.Length)

$MethodAnchor = '    void PrepareRenderState(const GraphicsPipeline* pipeline);'
$MethodIndex = $Text.IndexOf($MethodAnchor)
if ($MethodIndex -lt 0 -or $Text.IndexOf($MethodAnchor, $MethodIndex + 1) -ge 0) {
    throw 'Build 17 transform failed: Rasterizer private method anchor was not found exactly once'
}
$Methods = @'
    // BUILD 17 diagnostics only. No compute work is submitted by these methods.
    void RecordSafeGpuComputeForensics(std::string_view dispatch_type,
                                       VAddr indirect_address, u32 indirect_size);
    void FlushSafeGpuComputeForensics() const;

'@
$Text = $Text.Substring(0, $MethodIndex) + $Methods + $Text.Substring($MethodIndex)

$MemberAnchor = '    u64 safe_gpu_last_submitted_pipeline_hash{};'
$MemberIndex = $Text.IndexOf($MemberAnchor)
if ($MemberIndex -lt 0 -or $Text.IndexOf($MemberAnchor, $MemberIndex + 1) -ge 0) {
    throw 'Build 17 transform failed: BUILD 11 last-submitted member anchor was not found exactly once'
}
$MemberInsertAt = $MemberIndex + $MemberAnchor.Length
$Members = @'

    // BUILD 17 compute-forensics signatures -> occurrence counts.
    std::map<std::string, u64> safe_gpu_compute_forensics;
    u64 safe_gpu_compute_forensics_events{};
'@
$Text = $Text.Substring(0, $MemberInsertAt) + $Members + $Text.Substring($MemberInsertAt)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Implement the recorder before the existing BUILD 11 graphics-pipeline diagnostic method.
$Path = 'src/video_core/renderer_vulkan/vk_rasterizer.cpp'
$Text = Normalize([System.IO.File]::ReadAllText($Path))
$ImplAnchor = 'void Rasterizer::PersistLastSubmittedGraphicsPipeline('
$ImplIndex = $Text.IndexOf($ImplAnchor)
if ($ImplIndex -lt 0 -or $Text.IndexOf($ImplAnchor, $ImplIndex + 1) -ge 0) {
    throw 'Build 17 transform failed: BUILD 11 persistent graphics diagnostic anchor was not found exactly once'
}

$Implementation = @'
void Rasterizer::FlushSafeGpuComputeForensics() const {
    if (!safe_gpu_active ||
        VideoCore::SafeGpuGate::GetProfile() != VideoCore::SafeGpuProfile::Driveclub ||
        safe_gpu_compute_forensics.empty()) {
        return;
    }

    const auto directory = Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) /
                           "pipeline_forensics" / "compute_forensics";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        LOG_ERROR(Render_Vulkan,
                  "[SafeGPU][BUILD17] could not create compute-forensics directory: {}",
                  error.message());
        return;
    }

    const auto path = directory / "compute_dispatches.csv";
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        LOG_ERROR(Render_Vulkan,
                  "[SafeGPU][BUILD17] could not open compute_dispatches.csv");
        return;
    }

    output << "count,dispatch_type,queue_id,queue_class,pm4_command_address,shader_address,"
              "shader_hash,shader_crc32,shader_bytes,dim_x,dim_y,dim_z,start_x,start_y,start_z,"
              "threads_x,threads_y,threads_z,lds_bytes,user_regs,max_wave_id,dispatch_initiator,"
              "indirect_args_address,indirect_args_size,last_graphics_pipeline_hash,"
              "user_data_0,user_data_1,user_data_2,user_data_3,user_data_4,user_data_5,user_data_6,"
              "user_data_7,user_data_8,user_data_9,user_data_10,user_data_11,user_data_12,"
              "user_data_13,user_data_14,user_data_15\n";
    for (const auto& [signature, count] : safe_gpu_compute_forensics) {
        output << count << ',' << signature << '\n';
    }
    output.flush();
}

void Rasterizer::RecordSafeGpuComputeForensics(const std::string_view dispatch_type,
                                               const VAddr indirect_address,
                                               const u32 indirect_size) {
    if (!safe_gpu_active ||
        VideoCore::SafeGpuGate::GetProfile() != VideoCore::SafeGpuProfile::Driveclub) {
        return;
    }

    const auto& cs = liverpool->GetCsRegs();
    const auto* code = cs.Address<u32*>();
    if (!code) {
        return;
    }

    // SearchBinaryInfo is the same OrbShdr metadata path used by normal compute pipeline setup.
    // BUILD 17 only reads metadata/raw guest shader bytes; it never creates or submits compute work.
    const auto& binary = AmdGpu::SearchBinaryInfo(code);
    constexpr u32 MaxDiagnosticShaderBytes = 4u * 1024u * 1024u;
    if (!binary.Valid() || binary.length == 0 || binary.length > MaxDiagnosticShaderBytes) {
        LOG_WARNING(Render_Vulkan,
                    "[SafeGPU][BUILD17] invalid compute shader metadata address={:#x} length={}",
                    static_cast<u64>(cs.address) << 8, binary.length);
        return;
    }

    const s32 queue_id = liverpool->GetCurrentQueueId();
    const std::string_view queue_class = queue_id == AmdGpu::Liverpool::GfxQueueId ? "gfx" : "async";
    const VAddr shader_address = static_cast<VAddr>(cs.address) << 8;
    const VAddr command_address = liverpool->GetCurrentComputeCommandAddress();

    std::string signature = fmt::format(
        "{},{},{},0x{:016x},0x{:016x},0x{:016x},0x{:08x},{},{},{},{},{},{},{},{},{},{},{},{},{},{},0x{:08x},0x{:016x},{},0x{:016x}",
        dispatch_type, queue_id, queue_class, command_address, shader_address, binary.shader_hash,
        binary.crc32, binary.length, cs.dim_x, cs.dim_y, cs.dim_z, cs.start_x, cs.start_y,
        cs.start_z, cs.num_thread_x.full, cs.num_thread_y.full, cs.num_thread_z.full,
        cs.SharedMemSize(), cs.settings.num_user_regs, cs.max_wave_id, cs.dispatch_initiator,
        indirect_address, indirect_size, safe_gpu_last_submitted_pipeline_hash);
    for (const u32 value : cs.user_data) {
        signature += fmt::format(",0x{:08x}", value);
    }

    auto [it, inserted] = safe_gpu_compute_forensics.try_emplace(std::move(signature), 0);
    ++it->second;
    const u64 event_count = ++safe_gpu_compute_forensics_events;

    if (inserted) {
        const auto directory = Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) /
                               "pipeline_forensics" / "compute_forensics" / "compute_shaders";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (!error) {
            const auto shader_path = directory / fmt::format("{:016x}.bin", binary.shader_hash);
            if (!std::filesystem::exists(shader_path)) {
                std::ofstream shader(shader_path, std::ios::binary | std::ios::out | std::ios::trunc);
                if (shader) {
                    shader.write(reinterpret_cast<const char*>(code), binary.length);
                }
            }
        }

        const u64 unique_count = safe_gpu_compute_forensics.size();
        if (unique_count <= 16 || ShouldLogSafeGpuSample(unique_count)) {
            LOG_WARNING(Render_Vulkan,
                        "[SafeGPU][BUILD17] COMPUTE_FORENSICS new signature={} shader={:#x} "
                        "queue={} dims={}x{}x{} total_unique={}",
                        dispatch_type, binary.shader_hash, queue_id, cs.dim_x, cs.dim_y, cs.dim_z,
                        unique_count);
        }
    }

    // Periodic rewrite makes the aggregate resilient to the known Driveclub no-save assertion.
    if (inserted || ShouldLogSafeGpuSample(event_count)) {
        FlushSafeGpuComputeForensics();
    }
}

'@
$Text = $Text.Substring(0, $ImplIndex) + $Implementation + $Text.Substring($ImplIndex)

# Record the workload immediately before the inherited SafeGPU compute gate returns. This leaves
# compute shader translation, descriptor binding, pipeline creation and dispatch fully disabled.
$DirectOld = @'
void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    if (safe_gpu_active && !VideoCore::SafeGpuGate::ShouldAllowCompute()) {
'@
$DirectNew = @'
void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    if (safe_gpu_active && !VideoCore::SafeGpuGate::ShouldAllowCompute()) {
        RecordSafeGpuComputeForensics("direct", 0, 0);
'@
$First = $Text.IndexOf((Normalize $DirectOld))
if ($First -lt 0 -or $Text.IndexOf((Normalize $DirectOld), $First + 1) -ge 0) {
    throw 'Build 17 transform failed: DispatchDirect SafeGPU gate was not found exactly once'
}
$OldNorm = Normalize $DirectOld
$NewNorm = Normalize $DirectNew
$Text = $Text.Substring(0, $First) + $NewNorm + $Text.Substring($First + $OldNorm.Length)

$IndirectOld = @'
void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    if (safe_gpu_active && !VideoCore::SafeGpuGate::ShouldAllowCompute()) {
'@
$IndirectNew = @'
void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    if (safe_gpu_active && !VideoCore::SafeGpuGate::ShouldAllowCompute()) {
        RecordSafeGpuComputeForensics("indirect", address + offset, size);
'@
$First = $Text.IndexOf((Normalize $IndirectOld))
if ($First -lt 0 -or $Text.IndexOf((Normalize $IndirectOld), $First + 1) -ge 0) {
    throw 'Build 17 transform failed: DispatchIndirect SafeGPU gate was not found exactly once'
}
$OldNorm = Normalize $IndirectOld
$NewNorm = Normalize $IndirectNew
$Text = $Text.Substring(0, $First) + $NewNorm + $Text.Substring($First + $OldNorm.Length)

# Ensure a normal shutdown writes final counts too.
$DestructorOld = @'
Rasterizer::~Rasterizer() {
    if (safe_gpu_active) {
        LogSafeGpuSummary();
    }
}
'@
$DestructorNew = @'
Rasterizer::~Rasterizer() {
    if (safe_gpu_active) {
        FlushSafeGpuComputeForensics();
        LogSafeGpuSummary();
    }
}
'@
$First = $Text.IndexOf((Normalize $DestructorOld))
if ($First -lt 0 -or $Text.IndexOf((Normalize $DestructorOld), $First + 1) -ge 0) {
    throw 'Build 17 transform failed: Rasterizer destructor was not found exactly once'
}
$OldNorm = Normalize $DestructorOld
$NewNorm = Normalize $DestructorNew
$Text = $Text.Substring(0, $First) + $NewNorm + $Text.Substring($First + $OldNorm.Length)
[System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)

# Fail closed verification: compute policy remains disabled and all diagnostic markers exist.
$SafeGpuCpp = Normalize([System.IO.File]::ReadAllText('src/video_core/safe_gpu/safe_gpu.cpp'))
if (-not $SafeGpuCpp.Contains('bool SafeGpuGate::ShouldAllowCompute() noexcept') -or
    -not $SafeGpuCpp.Contains('return false;')) {
    throw 'Build 17 transform failed verification: inherited fail-closed compute policy could not be verified'
}
$RasterVerify = Normalize([System.IO.File]::ReadAllText('src/video_core/renderer_vulkan/vk_rasterizer.cpp'))
foreach ($Marker in @('[SafeGPU][BUILD17] COMPUTE_FORENSICS', 'compute_dispatches.csv',
                       'RecordSafeGpuComputeForensics("direct"',
                       'RecordSafeGpuComputeForensics("indirect"')) {
    if (-not $RasterVerify.Contains($Marker)) {
        throw "Build 17 transform failed verification: marker missing: $Marker"
    }
}
$LiverpoolVerify = Normalize([System.IO.File]::ReadAllText('src/video_core/amdgpu/liverpool.cpp'))
if ((Count-Occurrences $LiverpoolVerify 'BUILD 17 diagnostic only: preserve guest PM4 packet address') -ne 4) {
    throw 'Build 17 transform failed verification: not all four PM4 compute parser sites were instrumented'
}

Write-Host 'Build 17 transform verified: Driveclub compute workload metadata/raw shaders are persisted while all SafeGPU compute execution remains disabled.'
