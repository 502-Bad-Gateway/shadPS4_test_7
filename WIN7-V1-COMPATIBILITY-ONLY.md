# Windows 7 V1: compatibility-only validation

This branch starts from upstream shadPS4 commit
`7fb1a530c15415097836521fec6f3483e27c81ae` and carries the Windows/CRT
portability, legacy address-space support, and Vulkan 1.2 host-presenter bridge
needed for Windows 7 validation.

Guest GPU/Vulkan experiments remain intentionally absent. The current
settings-precedence axis defaults to the Vulkan path so the launcher and
emulator can prove that explicit global and per-game choices are honored.

## Effective defaults and overrides

When `ENABLE_WINDOWS_7_COMPAT=ON`, CMake defaults
`ENABLE_WINDOWS_7_COMPAT_ONLY=ON`. That mode supplies these factory defaults:

```text
null_gpu=false
show_fps_counter=true
show_splash=true
```

These are defaults, not forced runtime values. The emulator honors explicit
global configuration and per-game overrides after loading them. At game start,
the log reports the final effective values with the marker
`Windows 7 compatibility-only settings active`.

## Required checks

| Game | Title ID |
| --- | --- |
| Bloodborne | CUSA03173 |
| Driveclub | CUSA00003 |
| Dead or Alive Xtreme 3 Fortune | CUSA04555 |
| Wipeout Omega Collection | CUSA05670 |
| We Are Doomed (control) | CUSA02394 |

For the settings-axis acceptance test, confirm the startup marker reports the
requested `null_gpu` value for the factory default, global setting, and
per-game override. Re-run Bloodborne and Driveclub with `null_gpu=true` as
regression controls for the completed compatibility milestone.

Only results whose startup marker reports `null_gpu=false` may be used to
judge guest GPU/Vulkan behavior.

## Build

The dedicated GitHub Actions workflow builds the emulator with clang-cl using
the MSVC/UCRT ABI and target `x86_64-pc-windows-msvc`. The small Windows 7
launcher remains a separate static LLVM-MinGW/MSVCRT executable. The workflow
uses `x86-64-v3` and uploads both executables, PE/import audits, compiler
identity, hashes, and build identity as one artifact.
