// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/error.h"
#include "common/exit.h"
#include "common/signal_context.h"
#include "common/string_util.h"
#include "core/cpu_patches.h" // Windows static guest red-zone protection
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
#ifdef SHADPS4_WINDOWS_7_COMPAT
#include <dbghelp.h>
#endif
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <pthread.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

namespace Core {

#if defined(_WIN32)

#ifdef SHADPS4_WINDOWS_7_COMPAT
namespace {

std::atomic_flag crash_dump_started = ATOMIC_FLAG_INIT;

void LogWindows7ExceptionDetails(const EXCEPTION_POINTERS* exception) noexcept {
    if (exception == nullptr || exception->ExceptionRecord == nullptr) {
        return;
    }

    const auto* record = exception->ExceptionRecord;
    const auto instruction = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    const ULONG_PTR operation =
        record->NumberParameters >= 1 ? record->ExceptionInformation[0] : ULONG_PTR(-1);
    const auto fault_address = record->NumberParameters >= 2
                                   ? reinterpret_cast<void*>(record->ExceptionInformation[1])
                                   : nullptr;
    const char* access = operation == 0   ? "read"
                         : operation == 1 ? "write"
                         : operation == 8 ? "execute"
                                          : "unknown";

    LOG_CRITICAL(Debug,
                 "Windows 7 exception details: thread={}, access={}, fault_address={}, "
                 "parameters={}",
                 GetCurrentThreadId(), access, fault_address, record->NumberParameters);

    if (fault_address != nullptr) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(fault_address, &memory, sizeof(memory)) != 0) {
            LOG_CRITICAL(Debug,
                         "Fault memory: base={}, allocation_base={}, region_size={:#x}, "
                         "state={:#x}, protect={:#x}, allocation_protect={:#x}, type={:#x}",
                         memory.BaseAddress, memory.AllocationBase,
                         static_cast<u64>(memory.RegionSize), memory.State, memory.Protect,
                         memory.AllocationProtect, memory.Type);
        } else {
            LOG_CRITICAL(Debug, "VirtualQuery failed for fault address {}: {}", fault_address,
                         Common::GetLastErrorMsg());
        }
    }

    HMODULE module{};
    constexpr DWORD module_flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (record->ExceptionAddress != nullptr &&
        GetModuleHandleExW(module_flags, reinterpret_cast<LPCWSTR>(record->ExceptionAddress),
                           &module)) {
        wchar_t module_path[32768]{};
        const DWORD path_size = GetModuleFileNameW(module, module_path, 32768);
        const auto module_base = reinterpret_cast<uintptr_t>(module);
        if (path_size != 0) {
            LOG_CRITICAL(Debug, "Fault module: {} + {:#x} (base={})",
                         Common::UTF16ToUTF8(std::wstring_view{module_path, path_size}),
                         instruction - module_base, reinterpret_cast<void*>(module));
        } else {
            LOG_CRITICAL(Debug, "Fault module base={} offset={:#x}; path lookup failed: {}",
                         reinterpret_cast<void*>(module), instruction - module_base,
                         Common::GetLastErrorMsg());
        }
    } else {
        LOG_CRITICAL(Debug, "No loaded host module contains instruction {}",
                     record->ExceptionAddress);

        // Guest code is mapped directly into the process and therefore is not represented by a
        // Windows host module. Resolve it against the PS4 module list while a guest thread is
        // active so crash reports contain a stable module-relative address across ASLR runs.
        if (Libraries::Kernel::g_curthread != nullptr) {
            auto* linker = Common::Singleton<Core::Linker>::Instance();
            if (auto* guest_module = linker->FindByAddress(instruction); guest_module != nullptr) {
                const auto guest_base = guest_module->GetBaseAddress();
                LOG_CRITICAL(Debug, "Guest fault module: {} + {:#x} (base={:#x}, size={:#x})",
                             guest_module->name, instruction - guest_base, guest_base,
                             guest_module->aligned_base_size);
            }
        }
    }

#if defined(_M_X64) || defined(__x86_64__)
    if (exception->ContextRecord != nullptr) {
        const auto* context = exception->ContextRecord;
        LOG_CRITICAL(Debug, "Registers: RIP={:#x} RSP={:#x} RBP={:#x} EFLAGS={:#x}", context->Rip,
                     context->Rsp, context->Rbp, context->EFlags);
        LOG_CRITICAL(Debug, "Registers: RAX={:#x} RBX={:#x} RCX={:#x} RDX={:#x}", context->Rax,
                     context->Rbx, context->Rcx, context->Rdx);
        LOG_CRITICAL(Debug, "Registers: RSI={:#x} RDI={:#x} R8={:#x} R9={:#x}", context->Rsi,
                     context->Rdi, context->R8, context->R9);
        LOG_CRITICAL(Debug, "Registers: R10={:#x} R11={:#x} R12={:#x} R13={:#x}", context->R10,
                     context->R11, context->R12, context->R13);
        LOG_CRITICAL(Debug, "Registers: R14={:#x} R15={:#x}", context->R14, context->R15);
    }
#endif
}

void WriteWindows7MiniDump(EXCEPTION_POINTERS* exception) noexcept {
    if (exception == nullptr || crash_dump_started.test_and_set(std::memory_order_relaxed)) {
        return;
    }

    constexpr wchar_t dump_name[] = L"shadps4-crash.dmp";
    const HANDLE dump_file = CreateFileW(dump_name, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump_file == INVALID_HANDLE_VALUE) {
        LOG_CRITICAL(Debug, "Unable to create shadps4-crash.dmp: {}", Common::GetLastErrorMsg());
        return;
    }

    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (dbghelp == nullptr) {
        LOG_CRITICAL(Debug, "Unable to load dbghelp.dll for crash dump: {}",
                     Common::GetLastErrorMsg());
        CloseHandle(dump_file);
        return;
    }

    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
        HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, const MINIDUMP_EXCEPTION_INFORMATION*,
        const MINIDUMP_USER_STREAM_INFORMATION*, const MINIDUMP_CALLBACK_INFORMATION*);
    const auto write_dump =
        reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (write_dump == nullptr) {
        LOG_CRITICAL(Debug, "dbghelp.dll has no MiniDumpWriteDump export: {}",
                     Common::GetLastErrorMsg());
        FreeLibrary(dbghelp);
        CloseHandle(dump_file);
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION dump_exception{
        .ThreadId = GetCurrentThreadId(),
        .ExceptionPointers = exception,
        .ClientPointers = FALSE,
    };
    const BOOL written = write_dump(GetCurrentProcess(), GetCurrentProcessId(), dump_file,
                                    MiniDumpNormal, &dump_exception, nullptr, nullptr);
    const DWORD dump_error = written ? ERROR_SUCCESS : GetLastError();
    FlushFileBuffers(dump_file);
    FreeLibrary(dbghelp);
    CloseHandle(dump_file);

    if (written) {
        wchar_t full_path[32768]{};
        const DWORD path_size = GetFullPathNameW(dump_name, 32768, full_path, nullptr);
        if (path_size != 0 && path_size < 32768) {
            LOG_CRITICAL(Debug, "Crash minidump written to {}",
                         Common::UTF16ToUTF8(std::wstring_view{full_path, path_size}));
        } else {
            LOG_CRITICAL(Debug, "Crash minidump written to shadps4-crash.dmp");
        }
    } else {
        LOG_CRITICAL(Debug, "MiniDumpWriteDump failed: {}",
                     Common::NativeErrorToString(static_cast<int>(dump_error)));
    }
}

} // namespace
#endif

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    using namespace Libraries::Kernel;
    const auto* signals = Signals::Instance();
    // Windows static guest red-zone protection
    const bool use_static_windows_guest_red_zone_protection =
        WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    Ucontext guest_context{pExp->ContextRecord};
    Siginfo guest_info{
        ._si_signo = 0,
        ._si_errno = 0,
        ._si_code = POSIX_SI_NOINFO,
        ._si_addr = (void*)guest_context.uc_mcontext.mc_rip,
    };

    bool handled = false;
    bool static_protection_exception = false; // Windows static guest red-zone protection
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        guest_info._si_signo = POSIX_SIGSEGV;
        guest_info._si_code = POSIX_SEGV_MAPERR;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_ILLOPC;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case EXCEPTION_PRIV_INSTRUCTION: // Windows static guest red-zone protection
        if (use_static_windows_guest_red_zone_protection) {
            static_protection_exception = true;
            handled = signals->DispatchIllegalInstruction(pExp);
        }
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        guest_info._si_signo = POSIX_SIGBUS;
        guest_info._si_code = POSIX_BUS_ADRALN;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTDIV;
        break;
    case EXCEPTION_INT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTOVF;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTDIV;
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTINV;
        break;
    case EXCEPTION_FLT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTOVF;
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTUND;
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTSUB; // i am not sure about this one
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTRES;
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_BADSTK; // i am not sure about this one either
        break;
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        guest_info._si_signo = POSIX_SIGTRAP;
        guest_info._si_code = POSIX_TRAP_BRKPT;
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (guest_info._si_signo != 0) {
        if (g_curthread &&
            g_curthread->DispatchSignal(guest_info._si_signo, &guest_info, &guest_context)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // Windows static guest red-zone protection
    const bool report_unhandled = use_static_windows_guest_red_zone_protection
                                      ? static_protection_exception
                                      : code != EXCEPTION_BREAKPOINT;
    if (report_unhandled) { // Windows static guest red-zone protection
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
#ifdef SHADPS4_WINDOWS_7_COMPAT
        LogWindows7ExceptionDetails(pExp);
        WriteWindows7MiniDump(pExp);
#endif
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

static s32 NativeSiCodeToGuest(s32 sig, s32 code) {
    using namespace Libraries::Kernel;
    switch (sig) {
    case SIGUSR1:
        return POSIX_SI_LWP;
    case SIGSEGV:
        switch (code) {
        case SEGV_MAPERR:
            return POSIX_SEGV_MAPERR;
        case SEGV_ACCERR:
            return POSIX_SEGV_ACCERR;
        }
    case SIGBUS:
        switch (code) {
        case BUS_ADRALN:
            return POSIX_BUS_ADRALN;
        case BUS_ADRERR:
            return POSIX_BUS_ADRERR;
        case BUS_OBJERR:
            return POSIX_BUS_OBJERR;
        }
    case SIGILL:
        switch (code) {
        case ILL_ILLOPC:
            return POSIX_ILL_ILLOPC;
        case ILL_ILLOPN:
            return POSIX_ILL_ILLOPN;
        case ILL_ILLADR:
            return POSIX_ILL_ILLADR;
        case ILL_ILLTRP:
            return POSIX_ILL_ILLTRP;
        case ILL_PRVOPC:
            return POSIX_ILL_PRVOPC;
        case ILL_PRVREG:
            return POSIX_ILL_PRVREG;
        case ILL_COPROC:
            return POSIX_ILL_COPROC;
        case ILL_BADSTK:
            return POSIX_ILL_BADSTK;
        }
    case SIGFPE:
        switch (code) {
        case FPE_INTOVF:
            return POSIX_FPE_INTOVF;
        case FPE_INTDIV:
            return POSIX_FPE_INTDIV;
        case FPE_FLTDIV:
            return POSIX_FPE_FLTDIV;
        case FPE_FLTOVF:
            return POSIX_FPE_FLTOVF;
        case FPE_FLTUND:
            return POSIX_FPE_FLTUND;
        case FPE_FLTRES:
            return POSIX_FPE_FLTRES;
        case FPE_FLTINV:
            return POSIX_FPE_FLTINV;
        case FPE_FLTSUB:
            return POSIX_FPE_FLTSUB;
        }
    case SIGTRAP:
        switch (code) {
        case TRAP_BRKPT:
            return POSIX_TRAP_BRKPT;
        case TRAP_TRACE:
            return POSIX_TRAP_TRACE;
#ifdef __FreeBSD__
        case TRAP_DTRACE:
            return POSIX_TRAP_DTRACE;
#endif
        }

    default:
        return POSIX_SI_NOINFO;
    }
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    using namespace Libraries::Kernel;
    auto* thread = g_curthread;
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    Ucontext context{info, reinterpret_cast<ucontext_t*>(raw_context)};
    Siginfo guest_info{};
    if (info) {
        guest_info = *reinterpret_cast<Siginfo*>(info);
        guest_info._si_signo = sig == SIGUSR1 ? 0 : NativeToOrbisSignal(info->si_signo);
        guest_info._si_errno = NativeToPosixErrno(info->si_errno);
        guest_info._si_code = NativeSiCodeToGuest(sig, info->si_code);
        guest_info._si_addr = (void*)context.uc_mcontext.mc_rip;
    }
    Siginfo* info_p = info ? &guest_info : nullptr;
    Ucontext* context_p = raw_context ? &context : nullptr;

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        const bool is_write = Common::IsWriteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
                return;
            }
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (signals->DispatchIllegalInstruction(raw_context)) {
            return;
        }
    case SIGFPE:
    case SIGTRAP:
    case SIGSYS: {
        if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
            return;
        }

        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
    case SIGSLEEP: {
        // Sleep thread until signal is received again
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGSLEEP);
        sigwait(&sigset, &sig);
        break;
    }
    case SIGUSR1:
        if (thread) {
            thread->DispatchPendingSignals(info_p, context_p);
        }
        break;
    default:
        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
}

#endif

SignalDispatch::SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(
        sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
            sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
            sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
            sigaction(SIGUSR1, &action, nullptr) == 0 && sigaction(SIGSLEEP, &action, nullptr) == 0,
        "Failed to register signal handlers.");
#endif
}

void SignalDispatch::RemoveHandlers() {
    // asserting here would get into an infinite loop until too
    // many nested exceptions makes the OS kill the process
#if defined(_WIN32)
    if (!(RemoveVectoredExceptionHandler(handle))) {
        LOG_CRITICAL(Core, "Failed to remove exception handler.");
        Common::QuickExit(1);
    }
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    if (!(sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
          sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
          sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
          sigaction(SIGUSR1, &action, nullptr) == 0 &&
          sigaction(SIGSLEEP, &action, nullptr) == 0)) {
        LOG_CRITICAL(Core, "Failed to remove signal handlers.");
        Common::QuickExit(1);
    }
#endif
}

SignalDispatch::~SignalDispatch() {
    RemoveHandlers();
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
