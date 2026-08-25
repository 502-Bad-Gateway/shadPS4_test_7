// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <map>
#include <vector>
#include "common/alignment.h"
#include "common/arch.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/error.h"
#include "common/windows_compat.h"
#include "core/address_space.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/memory.h"
#include "core/memory.h"
#include "libraries/error_codes.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#endif

#if defined(__APPLE__) && defined(ARCH_X86_64)
// Reserve space for the system address space using a zerofill section.
asm(".zerofill SYSTEM_MANAGED,SYSTEM_MANAGED,__SYSTEM_MANAGED,0x7FFBFC000");
asm(".zerofill SYSTEM_RESERVED,SYSTEM_RESERVED,__SYSTEM_RESERVED,0x7C0004000");
asm(".zerofill USER_AREA,USER_AREA,__USER_AREA,0x5F9000000000");
#endif

namespace Core {

// Constants used for mapping address space.
constexpr VAddr SYSTEM_MANAGED_MIN = 0x400000ULL;
constexpr VAddr SYSTEM_MANAGED_MAX = 0x7FFFFBFFFULL;
constexpr VAddr SYSTEM_RESERVED_MIN = 0x7FFFFC000ULL;
#if defined(__APPLE__) && defined(ARCH_X86_64)
// Commpage ranges from 0xFC0000000 - 0xFFFFFFFFF, so decrease the system reserved maximum.
constexpr VAddr SYSTEM_RESERVED_MAX = 0xFBFFFFFFFULL;
// GPU-reserved memory ranges from 0x1000000000 - 0x6FFFFFFFFF, so increase the user minimum.
constexpr VAddr USER_MIN = 0x7000000000ULL;
#else
constexpr VAddr SYSTEM_RESERVED_MAX = 0xFFFFFFFFFULL;
constexpr VAddr USER_MIN = 0x1000000000ULL;
#endif
#if defined(__linux__)
// Linux maps the shadPS4 executable around here, so limit the user maximum
constexpr VAddr USER_MAX = 0x54FFFFFFFFFFULL;
#elif defined(__FreeBSD__)
// FreeBSD address space is extremely volatile, keep this lower for safety.
constexpr VAddr USER_MAX = 0xFFFFFFFFFFFULL;
#else
constexpr VAddr USER_MAX = 0x5FFFFFFFFFFFULL;
#endif

// Constants for the sizes of the ranges in address space.
static constexpr u64 SystemManagedSize = SYSTEM_MANAGED_MAX - SYSTEM_MANAGED_MIN + 1;
static constexpr u64 SystemReservedSize = SYSTEM_RESERVED_MAX - SYSTEM_RESERVED_MIN + 1;
static constexpr u64 UserSize = USER_MAX - USER_MIN + 1;

// Required backing file size for mapping physical address space.
static u64 BackingSize = ORBIS_KERNEL_TOTAL_MEM_DEV_PRO + ORBIS_KERNEL_FLEXIBLE_MEMORY_SIZE;

#ifdef _WIN32

[[nodiscard]] constexpr u64 ToWindowsProt(Core::MemoryProt prot) {
    const bool read =
        True(prot & Core::MemoryProt::CpuRead) || True(prot & Core::MemoryProt::GpuRead);
    const bool write =
        True(prot & Core::MemoryProt::CpuWrite) || True(prot & Core::MemoryProt::GpuWrite);
    const bool execute = True(prot & Core::MemoryProt::CpuExec);

    if (write && !read) {
        // While write-only CPU mappings aren't possible, write-only GPU mappings are.
        LOG_WARNING(Core, "Converting write-only mapping to read-write");
    }

    // All cases involving execute permissions have separate permissions.
    if (execute) {
        if (write) {
            return PAGE_EXECUTE_READWRITE;
        } else if (read && !write) {
            return PAGE_EXECUTE_READ;
        } else {
            return PAGE_EXECUTE;
        }
    } else {
        if (write) {
            return PAGE_READWRITE;
        } else if (read && !write) {
            return PAGE_READONLY;
        } else {
            return PAGE_NOACCESS;
        }
    }
}

PVOID ModernVirtualAlloc(HANDLE process, PVOID base_address, SIZE_T size, ULONG allocation_type,
                         ULONG page_protection) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
    return Common::Windows::VirtualAlloc2(process, base_address, size, allocation_type,
                                          page_protection);
#else
    return VirtualAlloc2(process, base_address, size, allocation_type, page_protection, nullptr, 0);
#endif
}

HANDLE ModernCreateFileMapping(HANDLE file, SECURITY_ATTRIBUTES* security_attributes,
                               ULONG desired_access, ULONG page_protection,
                               ULONG allocation_attributes, ULONGLONG maximum_size, PCWSTR name) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
    return Common::Windows::CreateFileMapping2(file, security_attributes, desired_access,
                                               page_protection, allocation_attributes, maximum_size,
                                               name);
#else
    return CreateFileMapping2(file, security_attributes, desired_access, page_protection,
                              allocation_attributes, maximum_size, name, nullptr, 0);
#endif
}

PVOID ModernMapViewOfFile(HANDLE file_mapping, HANDLE process, PVOID base_address,
                          ULONGLONG offset, SIZE_T view_size, ULONG allocation_type,
                          ULONG page_protection) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
    return Common::Windows::MapViewOfFile3(file_mapping, process, base_address, offset, view_size,
                                           allocation_type, page_protection);
#else
    return MapViewOfFile3(file_mapping, process, base_address, offset, view_size, allocation_type,
                          page_protection, nullptr, 0);
#endif
}

BOOL ModernUnmapViewOfFile(HANDLE process, PVOID base_address, ULONG unmap_flags) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
    return Common::Windows::UnmapViewOfFile2(process, base_address, unmap_flags);
#else
    return UnmapViewOfFile2(process, base_address, unmap_flags);
#endif
}

struct MemoryRegion {
    VAddr base;
    PAddr phys_base;
    u64 size;
    u32 prot;
    s32 fd;
    bool is_mapped;
};

#ifdef SHADPS4_WINDOWS_7_COMPAT
static constexpr u64 LegacyAllocationGranularity = 0x10000;
// Reserving the restricted 1 TiB guest range in very small views takes long enough for ordinary
// Windows allocations to claim the next not-yet-reserved address. Four GiB views reserve the
// initial range quickly; later section replacements install their target mapping before rebuilding
// and reprotecting the surrounding placeholder pieces, keeping the exposed interval short.
static constexpr u64 LegacyPlaceholderViewSize = 4_GB;

[[nodiscard]] constexpr DWORD ToLegacyMapAccess(ULONG protection) {
    switch (protection) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
        return FILE_MAP_READ | FILE_MAP_EXECUTE;
    case PAGE_EXECUTE_READWRITE:
        return FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_EXECUTE;
    case PAGE_READONLY:
        return FILE_MAP_READ;
    case PAGE_NOACCESS:
    case PAGE_READWRITE:
    default:
        return FILE_MAP_READ | FILE_MAP_WRITE;
    }
}

[[nodiscard]] constexpr bool IsLegacySectionBacked(const MemoryRegion& region) {
    // Read-only regular files are copied into anonymous placeholder pages. All other physical
    // mappings need a real section view so aliases remain coherent.
    return region.phys_base != PAddr(-1) && !(region.fd != -1 && region.prot == PAGE_READONLY);
}
#endif

struct AddressSpace::Impl {
    Impl() : process{GetCurrentProcess()} {
        // Determine the system's page alignment
        SYSTEM_INFO sys_info{};
        GetSystemInfo(&sys_info);
        u64 alignment = sys_info.dwAllocationGranularity;
#ifdef SHADPS4_WINDOWS_7_COMPAT
        allocation_granularity = alignment;
        ASSERT_MSG(allocation_granularity == LegacyAllocationGranularity,
                   "Unexpected Windows allocation granularity {:#x}", allocation_granularity);
        use_legacy = !Common::Windows::SupportsModernMemoryApis();
        if (use_legacy) {
            LOG_INFO(Core, "Windows memory backend: Windows 7 legacy section views");
        } else {
            LOG_INFO(Core, "Windows memory backend: modern placeholder APIs (upstream semantics)");
        }
#endif

        // Older Windows builds have a severe performance issue with VirtualAlloc2.
        // We need to get the host's Windows version, then determine if it needs a workaround.
        auto ntdll_handle = GetModuleHandleW(L"ntdll.dll");
        ASSERT_MSG(ntdll_handle, "Failed to retrieve ntdll handle");

        // Get the RtlGetVersion function
        s64(WINAPI * RtlGetVersion)(LPOSVERSIONINFOW);
        *(FARPROC*)&RtlGetVersion = GetProcAddress(ntdll_handle, "RtlGetVersion");
        ASSERT_MSG(RtlGetVersion, "failed to retrieve function pointer for RtlGetVersion");

        // Call RtlGetVersion
        RTL_OSVERSIONINFOW os_version_info{};
        RtlGetVersion(&os_version_info);

        u64 supported_user_max = USER_MAX;
        // This is the build number for Windows 11 22H2
        static constexpr s32 AffectedBuildNumber = 22621;

        // Higher PS4 firmware versions prevent higher address mappings too.
        s32 sdk_ver = Common::ElfInfo::Instance().CompiledSdkVer();
        if (os_version_info.dwBuildNumber <= AffectedBuildNumber ||
            sdk_ver >= Common::ElfInfo::FW_300) {
            supported_user_max = 0x10000000000ULL;
            // Only log the message if we're restricting the user max due to operating system.
            // Since higher compiled SDK versions also get reduced max, we don't need to log there.
            if (sdk_ver < Common::ElfInfo::FW_300) {
                LOG_WARNING(
                    Core,
                    "Older Windows version detected, reducing user max to {:#x} to avoid problems",
                    supported_user_max);
            }
        }

        // Determine the free address ranges we can access.
        VAddr next_addr = SYSTEM_MANAGED_MIN;
        MEMORY_BASIC_INFORMATION info{};
        while (next_addr <= supported_user_max) {
            ASSERT_MSG(VirtualQuery(reinterpret_cast<PVOID>(next_addr), &info, sizeof(info)),
                       "Failed to query memory information for address {:#x}", next_addr);

            // Ensure logic uses values aligned to bage boundaries.
            next_addr = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
            next_addr = Common::AlignUp(next_addr, alignment);

            // Prevent size from going past supported_user_max
            u64 size = info.RegionSize;
            if (next_addr > supported_user_max) {
                size -= (next_addr - supported_user_max);
            }
            size = Common::AlignDown(size, alignment);

            // Check for free memory areas
            // Restrict region size to avoid overly fragmenting the virtual memory space.
            if (info.State == MEM_FREE && info.RegionSize > 0x1000000) {
                VAddr addr = Common::AlignUp(reinterpret_cast<VAddr>(info.BaseAddress), alignment);
                regions.emplace(addr,
                                MemoryRegion{addr, PAddr(-1), size, PAGE_NOACCESS, -1, false});
            }
        }

        // Reserve all detected free regions.
#ifdef SHADPS4_WINDOWS_7_COMPAT
        if (use_legacy) {
            // Win7 has no placeholder allocations. A large pagefile section created with
            // SEC_RESERVE provides inaccessible, uncommitted views which can be split and rejoined
            // at the host's 64 KiB allocation granularity. Mapping each view at its guest-address
            // offset also keeps subsequently committed anonymous pages unique instead of aliasing
            // every placeholder.
            const u64 placeholder_size = Common::AlignUp(
                supported_user_max + allocation_granularity, allocation_granularity);
            placeholder_handle = CreateFileMappingW(
                INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE,
                static_cast<DWORD>(placeholder_size >> 32), static_cast<DWORD>(placeholder_size),
                nullptr);
            ASSERT_MSG(placeholder_handle, "Unable to create legacy placeholder section: {}",
                       Common::GetLastErrorMsg());
        }
        u64 legacy_placeholder_view_count = 0;
#endif
        for (auto region : regions) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
            if (use_legacy) {
                VAddr view_base = region.second.base;
                u64 remaining_size = region.second.size;
                while (remaining_size != 0) {
                    const u64 view_size = std::min(remaining_size, LegacyPlaceholderViewSize);
                    MapLegacyPlaceholderView(view_base, view_size);
                    legacy_placeholder_views.emplace(view_base, view_size);
                    view_base += view_size;
                    remaining_size -= view_size;
                    ++legacy_placeholder_view_count;
                }
                continue;
            }
#endif
            auto addr = static_cast<u8*>(ModernVirtualAlloc(
                process, reinterpret_cast<PVOID>(region.second.base), region.second.size,
                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS));
            // All marked regions should reserve fine since they're free.
            ASSERT_MSG(addr, "Unable to reserve virtual address space: {}",
                       Common::GetLastErrorMsg());
        }
#ifdef SHADPS4_WINDOWS_7_COMPAT
        if (use_legacy) {
            LOG_INFO(Core,
                     "Reserved Windows 7 guest address space in {} legacy views (maximum view "
                     "size {:#x})",
                     legacy_placeholder_view_count, LegacyPlaceholderViewSize);
        }
#endif

        // Set these constants to ensure code relying on them works.
        // These do not fully encapsulate the state of the address space.
        system_managed_base = reinterpret_cast<u8*>(regions.begin()->first);
        system_managed_size = SystemManagedSize - (regions.begin()->first - SYSTEM_MANAGED_MIN);
        system_reserved_base = reinterpret_cast<u8*>(SYSTEM_RESERVED_MIN);
        system_reserved_size = SystemReservedSize;
        user_base = reinterpret_cast<u8*>(USER_MIN);
        user_size = supported_user_max - USER_MIN - 1;

        // Increase BackingSize to account for config options.
        BackingSize += EmulatorSettings.GetExtraDmemInMBytes() * 1_MB +
                       EmulatorSettings.GetExtraFmemInMBytes() * 1_MB;

        // Allocate backing file that represents the total physical memory.
#ifdef SHADPS4_WINDOWS_7_COMPAT
        if (use_legacy) {
            backing_handle = CreateFileMappingW(
                INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                static_cast<DWORD>(BackingSize >> 32), static_cast<DWORD>(BackingSize), nullptr);
        } else
#endif
        {
            backing_handle = ModernCreateFileMapping(
                INVALID_HANDLE_VALUE, nullptr, FILE_MAP_ALL_ACCESS, PAGE_EXECUTE_READWRITE,
                SEC_COMMIT, BackingSize, nullptr);
        }

        ASSERT_MSG(backing_handle, "{}", Common::GetLastErrorMsg());
#ifdef SHADPS4_WINDOWS_7_COMPAT
        if (use_legacy) {
            // The guest ranges have already been reserved, so an automatically placed view cannot
            // collide with them. This canonical view backs aliased guest mappings.
            backing_base = static_cast<u8*>(MapViewOfFile(
                backing_handle, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0, BackingSize));
            ASSERT_MSG(backing_base, "Unable to map legacy physical backing: {}",
                       Common::GetLastErrorMsg());
        } else
#endif
        {
            // Allocate virtual memory for the backing file map as a placeholder.
            backing_base = static_cast<u8*>(
                ModernVirtualAlloc(process, nullptr, BackingSize,
                                   MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS));
            ASSERT_MSG(backing_base, "{}", Common::GetLastErrorMsg());

            // Map the backing placeholder. This will commit the pages.
            void* const ret = ModernMapViewOfFile(
                backing_handle, process, backing_base, 0, BackingSize, MEM_REPLACE_PLACEHOLDER,
                PAGE_EXECUTE_READWRITE);
            ASSERT_MSG(ret == backing_base, "{}", Common::GetLastErrorMsg());
        }
    }

    ~Impl() {
#ifdef SHADPS4_WINDOWS_7_COMPAT
        if (use_legacy) {
            for (const auto& [base, region] : regions) {
                if (region.is_mapped && IsLegacySectionBacked(region) &&
                    !UnmapViewOfFile(reinterpret_cast<void*>(base))) {
                    LOG_CRITICAL(Core, "Failed to unmap legacy guest section at {:#x}", base);
                }
            }
            for (const auto& [base, size] : legacy_placeholder_views) {
                if (!UnmapViewOfFile(reinterpret_cast<void*>(base))) {
                    LOG_CRITICAL(Core, "Failed to unmap legacy placeholder at {:#x}, size {:#x}",
                                 base, size);
                }
            }
            if (placeholder_handle && !CloseHandle(placeholder_handle)) {
                LOG_CRITICAL(Core, "Failed to close legacy placeholder section");
            }
        }
#endif
        if (virtual_base) {
            if (!VirtualFree(virtual_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(Core, "Failed to free virtual memory");
            }
        }
        if (backing_base) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
            if (use_legacy) {
                if (!UnmapViewOfFile(backing_base)) {
                    LOG_CRITICAL(Core, "Failed to unmap legacy physical backing");
                }
            } else {
                if (!ModernUnmapViewOfFile(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_CRITICAL(Core, "Failed to unmap backing memory placeholder");
                }
                if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                    LOG_CRITICAL(Core, "Failed to free backing memory");
                }
            }
#else
            if (!ModernUnmapViewOfFile(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                LOG_CRITICAL(Core, "Failed to unmap backing memory placeholder");
            }
            if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(Core, "Failed to free backing memory");
            }
#endif
        }
        if (!CloseHandle(backing_handle)) {
            LOG_CRITICAL(Core, "Failed to free backing memory file handle");
        }
    }

#ifdef SHADPS4_WINDOWS_7_COMPAT
    struct LegacyProtectionRange {
        VAddr base;
        u64 size;
        DWORD protection;
    };

    void* MapLegacyPlaceholderView(VAddr base, u64 size) {
        ASSERT_MSG(Common::Is64KBAligned(base) && Common::Is64KBAligned(size),
                   "Legacy placeholder is not 64 KiB aligned: base={:#x}, size={:#x}", base, size);
        void* const ptr =
            MapViewOfFileEx(placeholder_handle, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                            static_cast<DWORD>(base >> 32), static_cast<DWORD>(base), size,
                            reinterpret_cast<void*>(base));
        if (ptr != reinterpret_cast<void*>(base)) {
            const DWORD map_error = GetLastError();
            MEMORY_BASIC_INFORMATION obstruction{};
            VAddr obstruction_address = base;
            const VAddr end = base + size;
            while (obstruction_address < end &&
                   VirtualQuery(reinterpret_cast<void*>(obstruction_address), &obstruction,
                                sizeof(obstruction))) {
                if (obstruction.State != MEM_FREE) {
                    break;
                }
                obstruction_address =
                    reinterpret_cast<VAddr>(obstruction.BaseAddress) + obstruction.RegionSize;
            }
            ASSERT_MSG(false,
                       "Unable to map legacy placeholder at {:#x}, size {:#x}: {} "
                       "First non-free address={:#x}, allocation base={:#x}, region size={:#x}, "
                       "state={:#x}, protect={:#x}, type={:#x}",
                       base, size, Common::NativeErrorToString(map_error), obstruction_address,
                       reinterpret_cast<VAddr>(obstruction.AllocationBase), obstruction.RegionSize,
                       obstruction.State, obstruction.Protect, obstruction.Type);
        }
        return ptr;
    }

    void* MapLegacySectionView(const MemoryRegion& region) {
        HANDLE backing = region.fd != -1 ? reinterpret_cast<HANDLE>(region.fd) : backing_handle;
        void* const ptr =
            MapViewOfFileEx(backing, ToLegacyMapAccess(region.prot), region.phys_base >> 32,
                            static_cast<DWORD>(region.phys_base), region.size,
                            reinterpret_cast<void*>(region.base));
        if (ptr != reinterpret_cast<void*>(region.base)) {
            const DWORD map_error = GetLastError();
            MEMORY_BASIC_INFORMATION obstruction{};
            const bool queried = VirtualQuery(reinterpret_cast<void*>(region.base), &obstruction,
                                              sizeof(obstruction)) != 0;
            ASSERT_MSG(false,
                       "Unable to map legacy section at {:#x}, size {:#x}, physical {:#x}: {} "
                       "Target query: success={}, allocation base={:#x}, region size={:#x}, "
                       "state={:#x}, protect={:#x}, type={:#x}",
                       region.base, region.size, region.phys_base,
                       Common::NativeErrorToString(map_error), queried,
                       reinterpret_cast<VAddr>(obstruction.AllocationBase), obstruction.RegionSize,
                       obstruction.State, obstruction.Protect, obstruction.Type);
        }

        DWORD old_protection{};
        ASSERT_MSG(VirtualProtect(ptr, region.size, region.prot, &old_protection),
                   "Unable to protect legacy section at {:#x}, size {:#x}: {}", region.base,
                   region.size, Common::GetLastErrorMsg());
        return ptr;
    }

    std::vector<LegacyProtectionRange> CaptureLegacyProtections(VAddr base, u64 size) {
        std::vector<LegacyProtectionRange> protections;
        const VAddr end = base + size;
        VAddr address = base;
        while (address < end) {
            MEMORY_BASIC_INFORMATION info{};
            ASSERT_MSG(VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)),
                       "VirtualQuery failed for legacy view at {:#x}: {}", address,
                       Common::GetLastErrorMsg());
            const VAddr query_end =
                std::min(end, reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize);
            if (info.State == MEM_COMMIT) {
                protections.push_back({address, query_end - address, info.Protect});
            }
            address = query_end;
        }
        return protections;
    }

    void RestoreLegacyProtections(const std::vector<LegacyProtectionRange>& protections) {
        for (const auto& range : protections) {
            DWORD old_protection{};
            ASSERT_MSG(VirtualProtect(reinterpret_cast<void*>(range.base), range.size,
                                      range.protection, &old_protection),
                       "Unable to restore legacy protection at {:#x}, size {:#x}: {}", range.base,
                       range.size, Common::GetLastErrorMsg());
        }
    }

    void RestoreLegacyPlaceholderProtections(VAddr base, u64 size) {
        const VAddr end = base + size;
        auto region = std::prev(regions.upper_bound(base));
        for (; region != regions.end() && region->first < end; ++region) {
            const VAddr region_start = std::max(base, region->second.base);
            const VAddr region_end = std::min(end, region->second.base + region->second.size);
            const DWORD protection =
                region->second.is_mapped && !IsLegacySectionBacked(region->second)
                    ? region->second.prot
                    : PAGE_NOACCESS;

            VAddr address = region_start;
            while (address < region_end) {
                MEMORY_BASIC_INFORMATION info{};
                ASSERT_MSG(VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)),
                           "VirtualQuery failed for legacy placeholder at {:#x}: {}", address,
                           Common::GetLastErrorMsg());
                const VAddr query_end = std::min(
                    region_end, reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize);
                if (info.State == MEM_COMMIT) {
                    DWORD old_protection{};
                    ASSERT_MSG(VirtualProtect(reinterpret_cast<void*>(address), query_end - address,
                                              protection, &old_protection),
                               "Unable to restore legacy protection at {:#x}, size {:#x}: {}",
                               address, query_end - address, Common::GetLastErrorMsg());
                }
                address = query_end;
            }
        }
    }

    void CoalesceLegacyPlaceholderViews(VAddr base, VAddr end) {
        auto first = std::prev(legacy_placeholder_views.upper_bound(base));
        ASSERT_MSG(first->first <= base && first->first + first->second > base,
                   "No legacy placeholder contains address {:#x}", base);

        auto last = first;
        VAddr covered_end = last->first + last->second;
        while (covered_end < end) {
            const auto next = std::next(last);
            ASSERT_MSG(next != legacy_placeholder_views.end() && next->first == covered_end,
                       "Legacy placeholder range {:#x}-{:#x} crosses a section mapping", base, end);
            last = next;
            covered_end = last->first + last->second;
        }
        if (first == last) {
            return;
        }

        const VAddr combined_base = first->first;
        const u64 combined_size = last->first + last->second - combined_base;
        const auto protections = CaptureLegacyProtections(combined_base, combined_size);
        const auto after_last = std::next(last);
        for (auto view = first; view != after_last; ++view) {
            ASSERT_MSG(UnmapViewOfFile(reinterpret_cast<void*>(view->first)),
                       "Unable to unmap legacy placeholder at {:#x}: {}", view->first,
                       Common::GetLastErrorMsg());
        }
        MapLegacyPlaceholderView(combined_base, combined_size);
        first->second = combined_size;
        legacy_placeholder_views.erase(std::next(first), after_last);
        RestoreLegacyProtections(protections);
    }

    void EnsureLegacyPlaceholderRange(VAddr base, u64 size) {
        const VAddr aligned_base = Common::AlignDown(base, allocation_granularity);
        const VAddr aligned_end =
            Common::AlignUp(base + size, static_cast<std::size_t>(allocation_granularity));
        auto view = std::prev(legacy_placeholder_views.upper_bound(aligned_base));
        ASSERT_MSG(view->first <= aligned_base && view->first + view->second > aligned_base,
                   "No legacy placeholder covers {:#x}-{:#x}", aligned_base, aligned_end);

        VAddr covered_end = view->first + view->second;
        bool needs_coalesce = false;
        while (covered_end < aligned_end) {
            const auto next = std::next(view);
            ASSERT_MSG(next != legacy_placeholder_views.end() && next->first == covered_end,
                       "Legacy placeholder range {:#x}-{:#x} crosses a section mapping",
                       aligned_base, aligned_end);
            needs_coalesce = true;
            view = next;
            covered_end = view->first + view->second;
        }
        if (needs_coalesce) {
            // Coalesce only the views needed by this request. Expanding through every adjacent
            // view would recreate the single giant Win7 view that this compatibility path is
            // deliberately avoiding.
            CoalesceLegacyPlaceholderViews(aligned_base, aligned_end);
        }

        view = std::prev(legacy_placeholder_views.upper_bound(aligned_base));
        ASSERT_MSG(view->first <= aligned_base && view->first + view->second >= aligned_end,
                   "Unable to form legacy placeholder range {:#x}-{:#x}", aligned_base,
                   aligned_end);
    }

    void* ReplaceLegacyPlaceholderWithSection(MemoryRegion* region, u64 host_size) {
        const VAddr base = region->base;
        const u64 size = host_size;
        ASSERT_MSG(Common::Is64KBAligned(base) && Common::Is64KBAligned(size),
                   "Section-backed mappings on Windows 7 require 64 KiB virtual ranges: "
                   "base={:#x}, size={:#x}",
                   base, size);
        EnsureLegacyPlaceholderRange(base, size);

        auto view = std::prev(legacy_placeholder_views.upper_bound(base));
        const VAddr original_base = view->first;
        const u64 original_size = view->second;
        const VAddr original_end = original_base + original_size;
        const VAddr requested_end = base + size;
        ASSERT_MSG(original_base <= base && original_end >= requested_end,
                   "Legacy placeholder does not contain requested section mapping");

        const u64 prefix_size = base - original_base;
        const u64 suffix_size = original_end - requested_end;
        LOG_INFO(Core,
                 "Windows 7 section map {:#x}-{:#x}: replacing slice of legacy view "
                 "{:#x}-{:#x}",
                 base, requested_end, original_base, original_end);
        const auto prefix_protections = CaptureLegacyProtections(original_base, prefix_size);
        const auto suffix_protections = CaptureLegacyProtections(requested_end, suffix_size);
        if (prefix_size != 0) {
            view->second = prefix_size;
        }
        if (suffix_size != 0) {
            legacy_placeholder_views.emplace(requested_end, suffix_size);
        }

        ASSERT_MSG(UnmapViewOfFile(reinterpret_cast<void*>(original_base)),
                   "Unable to split legacy placeholder at {:#x}: {}", original_base,
                   Common::GetLastErrorMsg());
        if (prefix_size == 0) {
            legacy_placeholder_views.erase(view);
        }

        // Secure the fixed guest target before rebuilding either surrounding reservation. The V8
        // order restored and reprotected the prefix/suffix first, leaving this exact hole exposed
        // long enough for an unrelated host allocation to make MapViewOfFileEx fail.
        void* const target = MapLegacySectionView(*region);
        if (prefix_size != 0) {
            MapLegacyPlaceholderView(original_base, prefix_size);
        }
        if (suffix_size != 0) {
            MapLegacyPlaceholderView(requested_end, suffix_size);
        }
        RestoreLegacyProtections(prefix_protections);
        RestoreLegacyProtections(suffix_protections);
        return target;
    }

    void PrepareLegacyPlaceholder(VAddr base, u64 size) {
        ASSERT_MSG(Common::Is64KBAligned(base) && Common::Is64KBAligned(size),
                   "Cannot restore a non-64 KiB legacy section view: base={:#x}, size={:#x}", base,
                   size);
        const auto [view, inserted] = legacy_placeholder_views.emplace(base, size);
        ASSERT_MSG(inserted, "Legacy placeholder already exists at {:#x}, size {:#x}", base,
                   view->second);
    }

    void FinishLegacyPlaceholder(VAddr base, u64 size) {
        MapLegacyPlaceholderView(base, size);
        RestoreLegacyPlaceholderProtections(base, size);
    }
#endif

    void* MapRegion(MemoryRegion* region) {
        VAddr virtual_addr = region->base;
        PAddr phys_addr = region->phys_base;
        u64 size = region->size;
        ULONG prot = region->prot;
        s32 fd = region->fd;

        void* ptr = nullptr;
        if (phys_addr != -1) {
            HANDLE backing = fd != -1 ? reinterpret_cast<HANDLE>(fd) : backing_handle;
            if (fd != -1 && prot == PAGE_READONLY) {
                // Allocate the memory for the mapping
                DWORD resultvar;
#ifdef SHADPS4_WINDOWS_7_COMPAT
                if (use_legacy) {
                    ptr = VirtualAlloc(reinterpret_cast<PVOID>(virtual_addr), size, MEM_COMMIT,
                                       PAGE_READWRITE);
                } else {
                    ptr = ModernVirtualAlloc(
                        process, reinterpret_cast<PVOID>(virtual_addr), size,
                        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE);
                }
#else
                ptr = ModernVirtualAlloc(
                    process, reinterpret_cast<PVOID>(virtual_addr), size,
                    MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE);
#endif

                // Use ReadFile to read file contents into the memory area.
                // Create an OVERLAPPED with the file offset, then supply that to ReadFile
                OVERLAPPED param{};
                // Offset is the least-significant 32 bits, OffsetHigh is the most-significant.
                param.Offset = phys_addr & 0xffffffffull;
                param.OffsetHigh = (phys_addr & 0xffffffff00000000ull) >> 32;
                bool ret = ReadFile(backing, ptr, size, &resultvar, &param);
                ASSERT_MSG(ret, "ReadFile failed. {}", Common::GetLastErrorMsg());

                // ReadFile moves the file pointer, restore it with SetFilePointer
                s64 size_to_move = -size;
                LONG size_low = size_to_move & 0xffffffffull;
                LONG size_high = (size_to_move & 0xffffffff00000000ull) >> 32;
                ret = SetFilePointer(backing, size_low, &size_high, FILE_CURRENT);

                // Protect the memory area appropriately
                ret = VirtualProtect(ptr, size, prot, &resultvar);
                ASSERT_MSG(ret, "VirtualProtect failed. {}", Common::GetLastErrorMsg());
            } else {
#ifdef SHADPS4_WINDOWS_7_COMPAT
                if (use_legacy) {
                    ptr = MapLegacySectionView(*region);
                } else if (prot == PAGE_NOACCESS) {
                    DWORD resultvar;
                    ptr = ModernMapViewOfFile(backing, process,
                                              reinterpret_cast<PVOID>(virtual_addr), phys_addr,
                                              size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE);
                    ASSERT_MSG(ptr, "MapViewOfFile3 failed. {}", Common::GetLastErrorMsg());
                    bool ret = VirtualProtect(ptr, size, prot, &resultvar);
                    ASSERT_MSG(ret, "VirtualProtect failed. {}", Common::GetLastErrorMsg());
                } else {
                    ptr = ModernMapViewOfFile(backing, process,
                                              reinterpret_cast<PVOID>(virtual_addr), phys_addr,
                                              size, MEM_REPLACE_PLACEHOLDER, prot);
                    ASSERT_MSG(ptr, "MapViewOfFile3 failed. {}", Common::GetLastErrorMsg());
                }
#else
                if (prot == PAGE_NOACCESS) {
                    DWORD resultvar;
                    ptr = ModernMapViewOfFile(backing, process,
                                              reinterpret_cast<PVOID>(virtual_addr), phys_addr,
                                              size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE);
                    ASSERT_MSG(ptr, "MapViewOfFile3 failed. {}", Common::GetLastErrorMsg());
                    bool ret = VirtualProtect(ptr, size, prot, &resultvar);
                    ASSERT_MSG(ret, "VirtualProtect failed. {}", Common::GetLastErrorMsg());
                } else {
                    ptr = ModernMapViewOfFile(backing, process,
                                              reinterpret_cast<PVOID>(virtual_addr), phys_addr,
                                              size, MEM_REPLACE_PLACEHOLDER, prot);
                    ASSERT_MSG(ptr, "MapViewOfFile3 failed. {}", Common::GetLastErrorMsg());
                }
#endif
            }
        } else {
#ifdef SHADPS4_WINDOWS_7_COMPAT
            if (use_legacy) {
                // Anonymous guest pages are committed directly inside the SEC_RESERVE placeholder
                // view. Win7 cannot decommit an individual page in such a view, so reused pages are
                // explicitly zeroed and later hidden with PAGE_NOACCESS.
                ptr = VirtualAlloc(reinterpret_cast<PVOID>(virtual_addr), size, MEM_COMMIT,
                                   PAGE_READWRITE);
                ASSERT_MSG(ptr == reinterpret_cast<void*>(virtual_addr),
                           "Unable to commit legacy anonymous pages at {:#x}, size {:#x}: {}",
                           virtual_addr, size, Common::GetLastErrorMsg());
                std::memset(ptr, 0, size);
                if (prot != PAGE_READWRITE) {
                    DWORD old_protection{};
                    ASSERT_MSG(VirtualProtect(ptr, size, prot, &old_protection),
                               "Unable to protect legacy anonymous pages at {:#x}, size {:#x}: {}",
                               virtual_addr, size, Common::GetLastErrorMsg());
                }
            } else {
                ptr = ModernVirtualAlloc(
                    process, reinterpret_cast<PVOID>(virtual_addr), size,
                    MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, prot);
            }
#else
            ptr = ModernVirtualAlloc(process, reinterpret_cast<PVOID>(virtual_addr), size,
                                     MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, prot);
#endif
        }
        ASSERT_MSG(ptr == reinterpret_cast<void*>(virtual_addr), "{}", Common::GetLastErrorMsg());
        return ptr;
    }

    void UnmapRegion(const MemoryRegion* region) {
        VAddr virtual_addr = region->base;
        PAddr phys_base = region->phys_base;
        u64 size = region->size;
        ULONG prot = region->prot;
        s32 fd = region->fd;

        bool ret = false;
#ifdef SHADPS4_WINDOWS_7_COMPAT
        if (use_legacy) {
            if (IsLegacySectionBacked(*region)) {
                ret = UnmapViewOfFile(reinterpret_cast<PVOID>(virtual_addr));
            } else {
                DWORD old_protection{};
                ret = VirtualProtect(reinterpret_cast<PVOID>(virtual_addr), size, PAGE_NOACCESS,
                                     &old_protection);
            }
        } else if ((fd != -1 && prot != PAGE_READONLY) ||
                   (fd == -1 && phys_base != PAddr(-1))) {
            ret = ModernUnmapViewOfFile(process, reinterpret_cast<PVOID>(virtual_addr),
                                        MEM_PRESERVE_PLACEHOLDER);
        } else {
            ret = VirtualFreeEx(process, reinterpret_cast<PVOID>(virtual_addr), size,
                                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        }
#else
        if ((fd != -1 && prot != PAGE_READONLY) || (fd == -1 && phys_base != -1)) {
            ret = ModernUnmapViewOfFile(process, reinterpret_cast<PVOID>(virtual_addr),
                                        MEM_PRESERVE_PLACEHOLDER);
        } else {
            ret = VirtualFreeEx(process, reinterpret_cast<PVOID>(virtual_addr), size,
                                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        }
#endif
        ASSERT_MSG(ret, "Unmap on virtual_addr {:#x}, size {:#x} failed: {}", virtual_addr, size,
                   Common::GetLastErrorMsg());
    }

    void SplitRegion(VAddr virtual_addr, u64 size) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
        if (use_legacy) {
            auto it = std::prev(regions.upper_bound(virtual_addr));
            const MemoryRegion original = it->second;
            const VAddr original_end = original.base + original.size;
            const VAddr requested_end = virtual_addr + size;
            ASSERT_MSG(requested_end <= original_end, "Cannot fit region into one reservation");

            // Allocate every bookkeeping node before changing any section view. Anonymous mappings
            // remain committed inside their placeholder view and therefore need no host-side split.
            auto target = it;
            if (original.base != virtual_addr) {
                it->second.size = virtual_addr - original.base;
                const PAddr target_phys_base =
                    original.is_mapped ? original.phys_base + it->second.size : PAddr(-1);
                target = regions.emplace_hint(std::next(it), virtual_addr,
                                              MemoryRegion{virtual_addr, target_phys_base,
                                                           original_end - virtual_addr,
                                                           original.prot, original.fd,
                                                           original.is_mapped});
            }

            if (target->second.size != size) {
                const PAddr next_phys_base =
                    original.is_mapped ? target->second.phys_base + size : PAddr(-1);
                regions.emplace_hint(std::next(target), requested_end,
                                     MemoryRegion{requested_end, next_phys_base,
                                                  original_end - requested_end, original.prot,
                                                  original.fd, original.is_mapped});
                target->second.size = size;
            }

            if (original.is_mapped && IsLegacySectionBacked(original)) {
                ASSERT_MSG(
                    Common::Is64KBAligned(original.base) &&
                        Common::Is64KBAligned(virtual_addr) &&
                        (requested_end == original_end || Common::Is64KBAligned(requested_end)),
                    "Windows 7 cannot split a section-backed mapping at a 16 KiB-only "
                    "boundary: mapping={:#x}-{:#x}, split={:#x}-{:#x}",
                    original.base, original_end, virtual_addr, requested_end);

                const auto first_segment = regions.lower_bound(original.base);
                UnmapRegion(&original);
                for (auto segment = first_segment;
                     segment != regions.end() && segment->first < original_end; ++segment) {
                    MapRegion(&segment->second);
                }
            }
            return;
        }
#endif
        // First, get the region this range covers
        auto it = std::prev(regions.upper_bound(virtual_addr));

        // All unmapped areas will coalesce, so there should be a region
        // containing the full requested range. If not, then something is mapped here.
        ASSERT_MSG(it->second.base + it->second.size >= virtual_addr + size,
                   "Cannot fit region into one placeholder");

        // If the region is mapped, we need to unmap first before we can modify the placeholders.
        if (it->second.is_mapped) {
            ASSERT_MSG(it->second.phys_base != -1 || !it->second.is_mapped,
                       "Cannot split unbacked mapping");
            UnmapRegion(&it->second);
        }

        // We need to split this region to create a matching placeholder.
        if (it->second.base != virtual_addr) {
            // Requested address is not the start of the containing region,
            // create a new region to represent the memory before the requested range.
            auto& region = it->second;
            u64 base_offset = virtual_addr - region.base;
            u64 next_region_size = region.size - base_offset;
            PAddr next_region_phys_base = -1;
            if (region.is_mapped) {
                next_region_phys_base = region.phys_base + base_offset;
            }
            region.size = base_offset;

            // Use VirtualFreeEx to create the split.
            if (!VirtualFreeEx(process, LPVOID(region.base), region.size,
                               MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                UNREACHABLE_MSG("Region splitting failed: {}", Common::GetLastErrorMsg());
            }

            // If the mapping was mapped, remap the region.
            if (region.is_mapped) {
                MapRegion(&region);
            }

            // Store a new region matching the removed area.
            it = regions.emplace_hint(std::next(it), virtual_addr,
                                      MemoryRegion(virtual_addr, next_region_phys_base,
                                                   next_region_size, region.prot, region.fd,
                                                   region.is_mapped));
        }

        // At this point, the region's base will match virtual_addr.
        // Now check for a size difference.
        if (it->second.size != size) {
            // The requested size is smaller than the current region placeholder.
            // Update region to match the requested region,
            // then make a new region to represent the remaining space.
            auto& region = it->second;
            VAddr next_region_addr = region.base + size;
            u64 next_region_size = region.size - size;
            PAddr next_region_phys_base = -1;
            if (region.is_mapped) {
                next_region_phys_base = region.phys_base + size;
            }
            region.size = size;

            // Store the new region matching the remaining space
            regions.emplace_hint(std::next(it), next_region_addr,
                                 MemoryRegion(next_region_addr, next_region_phys_base,
                                              next_region_size, region.prot, region.fd,
                                              region.is_mapped));

            // Use VirtualFreeEx to create the split.
            if (!VirtualFreeEx(process, LPVOID(region.base), region.size,
                               MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                UNREACHABLE_MSG("Region splitting failed: {}", Common::GetLastErrorMsg());
            }

            // If these regions were mapped, then map the unmapped area beyond the requested range.
            if (region.is_mapped) {
                MapRegion(&std::next(it)->second);
            }
        }

        // If the requested region was mapped, remap it.
        if (it->second.is_mapped) {
            MapRegion(&it->second);
        }
    }

    void* Map(VAddr virtual_addr, PAddr phys_addr, u64 size, ULONG prot, s32 fd = -1) {
        std::scoped_lock lk{mutex};
        // Get a pointer to the region containing virtual_addr
        auto it = std::prev(regions.upper_bound(virtual_addr));

        // If needed, split surrounding regions to create a placeholder
        if (it->first != virtual_addr || it->second.size != size) {
            SplitRegion(virtual_addr, size);
            it = std::prev(regions.upper_bound(virtual_addr));
        }

        // Get the address and region for this range.
        auto& [base, region] = *it;
        ASSERT_MSG(!region.is_mapped, "Cannot overwrite mapped region");

        // Now we have a region matching the requested region, perform the actual mapping.
        region.is_mapped = true;
        region.phys_base = phys_addr;
        region.prot = prot;
        region.fd = fd;
#ifdef SHADPS4_WINDOWS_7_COMPAT
        if (use_legacy) {
            EnsureLegacyPlaceholderRange(region.base, region.size);
            if (IsLegacySectionBacked(region)) {
                ASSERT_MSG(Common::Is64KBAligned(region.base) &&
                               Common::Is64KBAligned(region.phys_base),
                           "Windows 7 section mappings require a 64 KiB-aligned virtual address "
                           "and physical offset: virtual={:#x}, physical={:#x}, size={:#x}",
                           region.base, region.phys_base, region.size);
                const u64 host_size =
                    Common::AlignUp(region.size, static_cast<std::size_t>(allocation_granularity));
                const VAddr padding_base = region.base + region.size;
                const VAddr padding_end = region.base + host_size;
                if (padding_base != padding_end) {
                    auto padding = std::prev(regions.upper_bound(padding_base));
                    for (; padding != regions.end() && padding->first < padding_end; ++padding) {
                        ASSERT_MSG(!padding->second.is_mapped,
                                   "Windows 7 host-allocation padding overlaps a guest mapping at "
                                   "{:#x}-{:#x}",
                                   padding_base, padding_end);
                    }
                }
                // Win7 has no atomic placeholder replacement operation. All map nodes have already
                // been allocated, and the exact target view is installed immediately after release.
                return ReplaceLegacyPlaceholderWithSection(&region, host_size);
            }
        }
#endif
        return MapRegion(&region);
    }

    void CoalesceFreeRegions(VAddr virtual_addr) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
        if (use_legacy) {
            auto it = std::prev(regions.upper_bound(virtual_addr));
            ASSERT_MSG(!it->second.is_mapped, "Cannot coalesce mapped regions");

            while (it != regions.begin()) {
                const auto previous = std::prev(it);
                if (previous->second.is_mapped ||
                    previous->first + previous->second.size != it->first) {
                    break;
                }
                previous->second.size += it->second.size;
                regions.erase(it);
                it = previous;
            }

            auto next = std::next(it);
            while (next != regions.end() && !next->second.is_mapped &&
                   it->first + it->second.size == next->first) {
                it->second.size += next->second.size;
                regions.erase(next);
                next = std::next(it);
            }
            return;
        }
#endif
        // First, get the region to update
        auto it = std::prev(regions.upper_bound(virtual_addr));
        ASSERT_MSG(!it->second.is_mapped, "Cannot coalesce mapped regions");

        // Check if there are adjacent free placeholders before this area.
        bool can_coalesce = false;
        auto it_prev = it != regions.begin() ? std::prev(it) : regions.end();
        while (it_prev != regions.end() && !it_prev->second.is_mapped &&
               it_prev->first + it_prev->second.size == it->first) {
            // If there is an earlier region, move our iterator to that and increase size.
            it_prev->second.size = it_prev->second.size + it->second.size;
            regions.erase(it);
            it = it_prev;

            // Mark this region as coalesce-able.
            can_coalesce = true;

            // Get the next previous region.
            it_prev = it != regions.begin() ? std::prev(it) : regions.end();
        }

        // Check if there are adjacent free placeholders after this area.
        auto it_next = std::next(it);
        while (it_next != regions.end() && !it_next->second.is_mapped &&
               it->first + it->second.size == it_next->first) {
            // If there is a later region, increase our current region's size
            it->second.size = it->second.size + it_next->second.size;
            regions.erase(it_next);

            // Mark this region as coalesce-able.
            can_coalesce = true;

            // Get the next region
            it_next = std::next(it);
        }

        // If there are placeholders to coalesce, then coalesce them.
        if (can_coalesce) {
            if (!VirtualFreeEx(process, LPVOID(it->first), it->second.size,
                               MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
                UNREACHABLE_MSG("Region coalescing failed: {}", Common::GetLastErrorMsg());
            }
        }
    }

    void Unmap(VAddr virtual_addr, u64 size) {
        std::scoped_lock lk{mutex};
        // Loop through all regions in the requested range
        u64 remaining_size = size;
        VAddr current_addr = virtual_addr;
        while (remaining_size > 0) {
            // Get a pointer to the region containing virtual_addr
            auto it = std::prev(regions.upper_bound(current_addr));

            // If necessary, split regions to ensure a valid unmap.
            // To prevent complication, ensure size is within the bounds of the current region.
            u64 base_offset = current_addr - it->second.base;
            u64 size_to_unmap = std::min<u64>(it->second.size - base_offset, remaining_size);
            if (current_addr != it->second.base || size_to_unmap != it->second.size) {
                SplitRegion(current_addr, size_to_unmap);
                it = std::prev(regions.upper_bound(current_addr));
            }

            // Get the address and region corresponding to this range.
            auto& [base, region] = *it;

            // Unmap the region if it was previously mapped
            if (region.is_mapped) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
                const bool section_backed = use_legacy && IsLegacySectionBacked(region);
                if (section_backed) {
                    // Allocate the map node while the section view still owns the address.
                    const u64 host_size = Common::AlignUp(
                        region.size, static_cast<std::size_t>(allocation_granularity));
                    PrepareLegacyPlaceholder(region.base, host_size);
                }
#endif
                UnmapRegion(&region);
#ifdef SHADPS4_WINDOWS_7_COMPAT
                if (section_backed) {
                    const u64 host_size = Common::AlignUp(
                        region.size, static_cast<std::size_t>(allocation_granularity));
                    FinishLegacyPlaceholder(region.base, host_size);
                }
#endif
                region.is_mapped = false;
                region.fd = -1;
                region.phys_base = -1;
                region.prot = PAGE_NOACCESS;
            } else {
                region.fd = -1;
                region.phys_base = -1;
                region.prot = PAGE_NOACCESS;
            }

            // Update loop variables
            remaining_size -= size_to_unmap;
            current_addr += size_to_unmap;
        }

        // Coalesce any free space produced from these unmaps.
        CoalesceFreeRegions(virtual_addr);
    }

    void Protect(VAddr virtual_addr, u64 size, bool read, bool write, bool execute) {
        std::scoped_lock lk{mutex};
        DWORD new_flags{};

        if (write && !read) {
            // While write-only CPU protection isn't possible, write-only GPU protection is.
            LOG_WARNING(Core, "Converting write-only protection to read-write");
        }

        // All cases involving execute permissions have separate permissions.
        if (execute) {
            // If there's some form of write protection requested, provide read-write permissions.
            if (write) {
                new_flags = PAGE_EXECUTE_READWRITE;
            } else if (read && !write) {
                new_flags = PAGE_EXECUTE_READ;
            } else {
                new_flags = PAGE_EXECUTE;
            }
        } else {
            if (write) {
                new_flags = PAGE_READWRITE;
            } else if (read && !write) {
                new_flags = PAGE_READONLY;
            } else {
                new_flags = PAGE_NOACCESS;
            }
        }

        // If no flags are assigned, then something's gone wrong.
        if (new_flags == 0) {
            LOG_CRITICAL(Core,
                         "Unsupported protection flag combination for address {:#x}, size {}, "
                         "read={}, write={}, execute={}",
                         virtual_addr, size, read, write, execute);
            return;
        }

        const VAddr virtual_end = virtual_addr + size;
        auto it = --regions.upper_bound(virtual_addr);
        ASSERT_MSG(it != regions.end(), "addr {:#x} out of bounds", virtual_addr);
        for (; it->first < virtual_end; it++) {
            if (!it->second.is_mapped) {
                continue;
            }
            const auto& region = it->second;
            const u64 range_addr = std::max(region.base, virtual_addr);
            const u64 range_size = std::min(region.base + region.size, virtual_end) - range_addr;
            DWORD old_flags{};
            if (!VirtualProtectEx(process, LPVOID(range_addr), range_size, new_flags, &old_flags)) {
                UNREACHABLE_MSG(
                    "Failed to change virtual memory protection for address {:#x}, size "
                    "{:#x}, error {}",
                    virtual_addr, size, Common::GetLastErrorMsg());
            }
        }
    }

    boost::icl::interval_set<VAddr> GetUsableRegions() {
        boost::icl::interval_set<VAddr> reserved_regions;
        for (auto region : regions) {
            reserved_regions.insert({region.second.base, region.second.base + region.second.size});
        }
        return reserved_regions;
    }

    [[nodiscard]] bool IsLegacyBackend() const noexcept {
#ifdef SHADPS4_WINDOWS_7_COMPAT
        return use_legacy;
#else
        return false;
#endif
    }

    std::mutex mutex;
    HANDLE process{};
    HANDLE backing_handle{};
#ifdef SHADPS4_WINDOWS_7_COMPAT
    bool use_legacy{};
    HANDLE placeholder_handle{};
    u64 allocation_granularity{};
    std::map<VAddr, u64> legacy_placeholder_views;
#endif
    u8* backing_base{};
    u8* virtual_base{};
    u8* system_managed_base{};
    u64 system_managed_size{};
    u8* system_reserved_base{};
    u64 system_reserved_size{};
    u8* user_base{};
    u64 user_size{};
    std::map<VAddr, MemoryRegion> regions;
};
#else

enum PosixPageProtection {
    PAGE_NOACCESS = 0,
    PAGE_READONLY = PROT_READ,
    PAGE_READWRITE = PROT_READ | PROT_WRITE,
    PAGE_EXECUTE = PROT_EXEC,
    PAGE_EXECUTE_READ = PROT_EXEC | PROT_READ,
    PAGE_EXECUTE_READWRITE = PROT_EXEC | PROT_READ | PROT_WRITE
};

[[nodiscard]] constexpr PosixPageProtection ToPosixProt(Core::MemoryProt prot) {
    const bool read =
        True(prot & Core::MemoryProt::CpuRead) || True(prot & Core::MemoryProt::GpuRead);
    const bool write =
        True(prot & Core::MemoryProt::CpuWrite) || True(prot & Core::MemoryProt::GpuWrite);
    const bool execute = True(prot & Core::MemoryProt::CpuExec);

    if (write && !read) {
        // While write-only CPU mappings aren't possible, write-only GPU mappings are.
        LOG_WARNING(Core, "Converting write-only mapping to read-write");
    }

    // All cases involving execute permissions have separate permissions.
    if (execute) {
        if (write) {
            return PAGE_EXECUTE_READWRITE;
        } else if (read && !write) {
            return PAGE_EXECUTE_READ;
        } else {
            return PAGE_EXECUTE;
        }
    } else {
        if (write) {
            return PAGE_READWRITE;
        } else if (read && !write) {
            return PAGE_READONLY;
        } else {
            return PAGE_NOACCESS;
        }
    }
}

struct AddressSpace::Impl {
    Impl() {
        BackingSize += EmulatorSettings.GetExtraDmemInMBytes() * 1_MB +
                       EmulatorSettings.GetExtraFmemInMBytes() * 1_MB;
        // Allocate virtual address placeholder for our address space.
        system_managed_size = SystemManagedSize;
        system_reserved_size = SystemReservedSize;
        user_size = UserSize;

        constexpr int protection_flags = PROT_READ | PROT_WRITE;
        int map_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED; // compiler knows its constexpr
#if !defined(__FreeBSD__)
        map_flags |= MAP_NORESERVE;
#endif

#if defined(__APPLE__) && defined(ARCH_X86_64)
        // On ARM64 Macs, we run into limitations due to the commpage from 0xFC0000000 - 0xFFFFFFFFF
        // and the GPU carveout region from 0x1000000000 - 0x6FFFFFFFFF. Because this creates gaps
        // in the available virtual memory region, we map memory space using three distinct parts.
        system_managed_base =
            reinterpret_cast<u8*>(mmap(reinterpret_cast<void*>(SYSTEM_MANAGED_MIN),
                                       system_managed_size, protection_flags, map_flags, -1, 0));
        system_reserved_base =
            reinterpret_cast<u8*>(mmap(reinterpret_cast<void*>(SYSTEM_RESERVED_MIN),
                                       system_reserved_size, protection_flags, map_flags, -1, 0));
        user_base = reinterpret_cast<u8*>(
            mmap(reinterpret_cast<void*>(USER_MIN), user_size, protection_flags, map_flags, -1, 0));
#else
        const auto virtual_size = system_managed_size + system_reserved_size + user_size;
#if defined(ARCH_X86_64) && !defined(__FreeBSD__)
        const auto virtual_base =
            reinterpret_cast<u8*>(mmap(reinterpret_cast<void*>(SYSTEM_MANAGED_MIN), virtual_size,
                                       protection_flags, map_flags, -1, 0));
        system_managed_base = virtual_base;
        system_reserved_base = reinterpret_cast<u8*>(SYSTEM_RESERVED_MIN);
        user_base = reinterpret_cast<u8*>(USER_MIN);
#else
        // FreeBSD can't stand MAP_FIXED or it may overwrite mmap() itself!
        // Map memory wherever possible and instruction translation can handle offsetting to the
        // base.
        map_flags &= ~MAP_FIXED;
        const auto virtual_base =
            reinterpret_cast<u8*>(mmap(nullptr, virtual_size, protection_flags, map_flags, -1, 0));
        system_managed_base = virtual_base;
        system_reserved_base = virtual_base + SYSTEM_RESERVED_MIN - SYSTEM_MANAGED_MIN;
        user_base = virtual_base + USER_MIN - SYSTEM_MANAGED_MIN;
#endif
#endif
        if (system_managed_base == MAP_FAILED || system_reserved_base == MAP_FAILED ||
            user_base == MAP_FAILED) {
            LOG_CRITICAL(Kernel_Vmm, "mmap failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }

        LOG_INFO(Kernel_Vmm, "System managed virtual memory region: {} - {}",
                 fmt::ptr(system_managed_base),
                 fmt::ptr(system_managed_base + system_managed_size - 1));
        LOG_INFO(Kernel_Vmm, "System reserved virtual memory region: {} - {}",
                 fmt::ptr(system_reserved_base),
                 fmt::ptr(system_reserved_base + system_reserved_size - 1));
        LOG_INFO(Kernel_Vmm, "User virtual memory region: {} - {}", fmt::ptr(user_base),
                 fmt::ptr(user_base + user_size - 1));

        const VAddr system_managed_addr = reinterpret_cast<VAddr>(system_managed_base);
        const VAddr system_reserved_addr = reinterpret_cast<VAddr>(system_managed_base);
        const VAddr user_addr = reinterpret_cast<VAddr>(user_base);
        m_free_regions.insert({system_managed_addr, system_managed_addr + system_managed_size});
        m_free_regions.insert({system_reserved_addr, system_reserved_addr + system_reserved_size});
        m_free_regions.insert({user_addr, user_addr + user_size});

#ifdef __APPLE__
        const auto shm_path = fmt::format("/BackingDmem{}", getpid());
        backing_fd = shm_open(shm_path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (backing_fd < 0) {
            LOG_CRITICAL(Kernel_Vmm, "shm_open failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }
        shm_unlink(shm_path.c_str());
#else
#ifndef __FreeBSD__
        madvise(virtual_base, virtual_size, MADV_HUGEPAGE);
#endif
        // NOTE: If you add MFD_HUGETLB or whatever, remember that FBSD will break (libc bug)
        // so please, do not, add MFD_* whatever unless you ifdef it away (must be 0 for FBSD)
        // using sized pages as well causes incessant vm_reclaim calls in kernel, do not use on FBSD
        // under any circumstances.
        backing_fd = memfd_create("BackingDmem", 0);
        if (backing_fd < 0) {
            LOG_CRITICAL(Kernel_Vmm, "memfd_create failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }
#endif

        // Defined to extend the file with zeros
        int ret = ftruncate(backing_fd, BackingSize);
        if (ret != 0) {
            LOG_CRITICAL(Kernel_Vmm, "ftruncate failed with {}, are you out-of-memory?",
                         strerror(errno));
            throw std::bad_alloc{};
        }

        // Map backing dmem handle.
        backing_base = static_cast<u8*>(
            mmap(nullptr, BackingSize, PROT_READ | PROT_WRITE, MAP_SHARED, backing_fd, 0));
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(Kernel_Vmm, "mmap failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }
    }

    void* Map(VAddr virtual_addr, PAddr phys_addr, u64 size, PosixPageProtection prot,
              int fd = -1) {
        m_free_regions.subtract({virtual_addr, virtual_addr + size});
#ifdef __APPLE__
        if ((prot & PROT_EXEC) != 0) {
            ASSERT_MSG(fd == -1, "Requested execute permissions for file mapping");
            phys_addr = -1;
        }
#endif
        const int handle = phys_addr != -1 ? (fd == -1 ? backing_fd : fd) : -1;
        const off_t host_offset = phys_addr != -1 ? phys_addr : 0;
        const int flag = phys_addr != -1 ? MAP_SHARED : (MAP_ANONYMOUS | MAP_PRIVATE);
        void* ret = mmap(reinterpret_cast<void*>(virtual_addr), size, prot, MAP_FIXED | flag,
                         handle, host_offset);
        ASSERT_MSG(ret != MAP_FAILED, "mmap failed: {}", strerror(errno));
        return ret;
    }

    void Unmap(VAddr virtual_addr, u64 size) {
        // Check to see if we are adjacent to any regions.
        VAddr start_address = virtual_addr;
        VAddr end_address = start_address + size;
        auto it = m_free_regions.find({start_address - 1, end_address + 1});

        // If we are, join with them, ensuring we stay in bounds.
        if (it != m_free_regions.end()) {
            start_address = std::min(start_address, it->lower());
            end_address = std::max(end_address, it->upper());
        }

        // Free the relevant region.
        m_free_regions.insert({start_address, end_address});

        // Return the adjusted pointers.
        void* ret = mmap(reinterpret_cast<void*>(start_address), end_address - start_address,
                         PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        ASSERT_MSG(ret != MAP_FAILED, "mmap failed: {}", strerror(errno));
    }

    void Protect(VAddr virtual_addr, u64 size, bool read, bool write, bool execute) {
        int flags = PROT_NONE;
        if (read) {
            flags |= PROT_READ;
        }
        if (write) {
            flags |= PROT_WRITE;
        }
#ifdef ARCH_X86_64
        if (execute) {
            flags |= PROT_EXEC;
        }
#endif
        int ret = mprotect(reinterpret_cast<void*>(virtual_addr), size, flags);
        ASSERT_MSG(ret == 0, "mprotect failed: {}", strerror(errno));
    }

    int backing_fd;
    u8* backing_base{};
    u8* system_managed_base{};
    u64 system_managed_size{};
    u8* system_reserved_base{};
    u64 system_reserved_size{};
    u8* user_base{};
    u64 user_size{};
    boost::icl::interval_set<VAddr> m_free_regions;
};
#endif

AddressSpace::AddressSpace() : impl{std::make_unique<Impl>()} {
    backing_base = impl->backing_base;
    system_managed_base = impl->system_managed_base;
    system_managed_size = impl->system_managed_size;
    system_reserved_base = impl->system_reserved_base;
    system_reserved_size = impl->system_reserved_size;
    user_base = impl->user_base;
    user_size = impl->user_size;
}

AddressSpace::~AddressSpace() = default;

void* AddressSpace::Map(VAddr virtual_addr, u64 size, PAddr phys_addr, bool is_exec) {
#if ARCH_X86_64
    const auto prot = is_exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
#else
    // On non-native architectures, we can simplify things by ignoring the execute flag for the
    // canonical copy of the memory and rely on the JIT to map translated code as executable.
    constexpr auto prot = PAGE_READWRITE;
#endif
    return impl->Map(virtual_addr, phys_addr, size, prot);
}

void* AddressSpace::MapFile(VAddr virtual_addr, u64 size, u64 offset, u32 prot, uintptr_t fd) {
#ifdef _WIN32
    return impl->Map(virtual_addr, offset, size,
                     ToWindowsProt(std::bit_cast<Core::MemoryProt>(prot)), fd);
#else
    return impl->Map(virtual_addr, offset, size, ToPosixProt(std::bit_cast<Core::MemoryProt>(prot)),
                     fd);
#endif
}

void AddressSpace::Unmap(VAddr virtual_addr, u64 size) {
    impl->Unmap(virtual_addr, size);
}

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    const bool read = True(perms & MemoryPermission::Read);
    const bool write = True(perms & MemoryPermission::Write);
    const bool execute = True(perms & MemoryPermission::Execute);
    return impl->Protect(virtual_addr, size, read, write, execute);
}

bool AddressSpace::IsLegacyBackend() const noexcept {
#ifdef _WIN32
    return impl->IsLegacyBackend();
#else
    return false;
#endif
}

boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
#ifdef _WIN32
    // On Windows, we need to obtain the accessible intervals from the implementation's regions.
    return impl->GetUsableRegions();
#else
    // On Linux and Mac, the memory space is fully represented by the three major regions
    boost::icl::interval_set<VAddr> reserved_regions;
    VAddr system_managed_addr = reinterpret_cast<VAddr>(system_managed_base);
    VAddr system_reserved_addr = reinterpret_cast<VAddr>(system_reserved_base);
    VAddr user_addr = reinterpret_cast<VAddr>(user_base);

    reserved_regions.insert({system_managed_addr, system_managed_addr + system_managed_size});
    reserved_regions.insert({system_reserved_addr, system_reserved_addr + system_reserved_size});
    reserved_regions.insert({user_addr, user_addr + user_size});
    return reserved_regions;
#endif
}

} // namespace Core
