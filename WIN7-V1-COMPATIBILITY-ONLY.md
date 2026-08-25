# Windows 7 V1: compatibility-only validation

This branch is the first isolation build for the Windows 7 work. It starts from
upstream shadPS4 commit `7fb1a530c15415097836521fec6f3483e27c81ae` and carries
only the Windows/CRT portability and legacy address-space changes needed to
run the emulator on Windows 7.

The Vulkan renderer and shader-recompiler experiments are intentionally absent.
The build defaults to the NullGPU path so a game reaching its in-game state is
evidence about Windows 7 compatibility, not about Vulkan support.

## Effective defaults

When `ENABLE_WINDOWS_7_COMPAT=ON`, CMake defaults
`ENABLE_WINDOWS_7_COMPAT_ONLY=ON`. The compatibility-only build forces these
effective settings after loading both global and per-game configuration:

```text
null_gpu=true
show_fps_counter=true
show_splash=true
```

This also prevents stale `config.json` or per-game profiles from silently
switching a test back to Vulkan. A later GPU/Vulkan phase can explicitly pass
`-DENABLE_WINDOWS_7_COMPAT_ONLY=OFF`.

## Required game checks

| Game | Title ID |
| --- | --- |
| Bloodborne | CUSA03173 |
| Driveclub | CUSA00003 |
| Dead or Alive Xtreme 3 Fortune | CUSA04555 |
| Wipeout Omega Collection | CUSA05670 |
| We Are Doomed (control) | CUSA02394 |

The success criterion is reaching the normal in-game state with the log
reporting `GPU isNullGpu: true`. A Vulkan crash, pipeline workaround, or
renderer assertion is outside this phase and must not be used to judge this
build.

## Build

The dedicated GitHub Actions workflow builds with llvm-mingw 20260616
(LLVM/Clang 22.1.8), x86-64-v3, and a Windows 7 PE target. It uploads the
emulator, the small Windows 7 launcher, runtime DLLs, PE import audit, and the
build identity file as one artifact.
