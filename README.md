# iOS Mach-O Loader Boilerplate for PS Vita

A reusable boilerplate and scaffolding framework for porting classic 32-bit iOS games (ARMv6 / ARMv7, 32-bit Mach-O `MH_EXECUTE`) to the PlayStation Vita.
This is the iOS counterpart to [`soloader-boilerplate`](https://github.com/Rinnegatamante/soloader-boilerplate) (Android), based on the proven, battle-tested architecture developed in [`Inotia2-vita`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/Inotia2-vita).

---

## Loader Architecture

| Component | File(s) | Role & Emulation Functionality |
|---|---|---|
| **Mach-O Loader** | [`source/macho.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/macho.c), [`source/macho.h`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/macho.h) | Custom static dyld and 32-bit Mach-O executable loader with fixed base address (`MACHO_SLIDE = 0x98000000`). Applies data segment rebases, resolves import tables (`__la_symbol_ptr`, `__nl_symbol_ptr`), and binds stubs for unhandled symbols. |
| **Assembly Glue & Trampolines** | [`source/asm.S`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/asm.S) | `call_guest` and `call_guest6` (preserve `r9`). `ml_align_call` (bridges iOS 4-byte stack alignment to VitaSDK/newlib 8-byte AAPCS). `ml_unimpl_entry` (auto-logging unimplemented stub). |
| **Darwin / POSIX Subsystem** | [`source/darwin.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/darwin.c), [`source/darwin.h`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/darwin.h) | Darwin libc, libgcc, pthreads, and sockets emulation. Thread-safe POSIX wrapper with `SceUID` tracking (prevents fatal crashes on `pthread_detach`). High-precision `mach_absolute_time()` mapped to `sceKernelGetProcessTimeWide()`. |
| **Variadic Formatter** | [`source/darwin_printf.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/darwin_printf.c), [`source/darwin_printf.h`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/darwin_printf.h) | Printf engine tailored to Darwin's ARM ABI: reads 64-bit doubles and integers from unaligned word boundaries, with native support for Objective-C's `%@` specifier (`NSString*`). |
| **Mini-runtime ObjC / UIKit** | [`source/objc.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/objc.c), [`source/objc.h`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/objc.h) | Fast selector-based dispatching with `objc_msgSend` and `objc_msgSend_stret`. Emulates `NSString`, `UIFont` (rendered using a FreeType glyph atlas), `NSBundle` (`resourcePath`), `NSCalendar`, `UIDevice`, and on-screen keyboard via `sceImeDialog`. |
| **OpenGL ES Graphics Backend** | [`source/gles.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/gles.c), [`source/gles.h`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/gles.h), [`source/vitagl_no_splash.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/vitagl_no_splash.c) | GLES1 and GLES2 pipeline powered by vendored `vitaGL` compiled with `SOFTFP_ABI=1`. Virtual FBO supports display orientation rotation and aspect-ratio scaling (portrait/landscape to Vita's native 960x544). Converts `GL_FIXED` vertex arrays and disables blocking splashscreens. |
| **CgBI PNG Decoder** | [`source/cgbi.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/cgbi.c), [`source/cgbi.h`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/cgbi.h) | Native decompressor and BGR-to-RGB byte-swap converter for Apple's proprietary CgBI-optimized PNG assets, allowing intact IPA bundles to load directly without offline conversion. |
| **CoreAudio Backend** | [`source/coreaudio.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/coreaudio.c) | Emulates AudioUnit RemoteIO and AudioSession on a dedicated high-priority `sceAudioOut` audio thread. |
| **Input Subsystem** | [`source/input.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/input.c), [`source/input.h`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/input.h) | Maps PS Vita physical buttons and front capacitive multitouch screen to iOS touch events (`UITouchPhaseBegan`, `Moved`, `Ended`). |
| **Logging Subsystem** | [`source/log.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/log.c), [`source/log.h`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/log.h) | Rotating on-device file logger (`ux0:data/<slug>/logs/log_001.log` .. `log_999.log`) and live stdout streaming. |
| **Game Patches & Glue** | [`source/patch.c`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/patch.c), [`source/patch.h`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/patch.h) | Dedicated hooks for game-specific memory patches, symbol redirection, and custom bypasses. |

---

## Critical iOS on PS Vita ABI Rules

1. **`r9` as a Scratch Register**:
   - In Apple's Darwin ARMv6/v7 ABI, `r9` is an available scratch register (not callee-saved).
   - In the standard ARM AAPCS (used by VitaSDK/newlib), `r9` is callee-saved or platform-reserved.
   - **Rule**: The entire loader codebase MUST be compiled with `-ffixed-r9`, and any execution transfer into the game binary must transition through `call_guest` / `call_guest6` in [`source/asm.S`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/source/asm.S) to prevent register corruption.
2. **SoftFP Calling Convention**:
   - iOS 32-bit binaries pass single and double precision floats in integer registers (`r0..r3`).
   - `vitaGL` MUST be compiled with `SOFTFP_ABI=1` so that internal calls and GXM commands (`sceGxmSetViewport`, etc.) maintain proper argument alignment.
3. **4-Byte Stack Alignment**:
   - 32-bit iOS code only enforces 4-byte stack alignment.
   - VitaSDK newlib functions (especially `printf`, `malloc`, and math routines) expect an 8-byte aligned stack (AAPCS).
   - All C callbacks invoked by the game binary enter through `ml_align_call` to re-align `sp` to an 8-byte boundary before calling newlib or vitaGL.
4. **FairPlay DRM Decryption**:
   - The Mach-O executable in the IPA must be completely decrypted (`cryptid = 0`).
   - Binaries dumped directly from an encrypted app package will crash immediately with a Data Abort when loaded.

---

## Included Tools (`tools/`)

- [`tools/macho_prep.py`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/tools/macho_prep.py): Analyzes thin or fat Mach-O executables, verifies `cryptid == 0`, scans `__DATA` pointers against symbols, and generates `rebase.bin`.
- [`tools/annotate.py`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/tools/annotate.py): Disassembles functions, resolving PIC calls, ObjC selectors, classes, and literal strings.
- [`tools/cgbi_check.py`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/tools/cgbi_check.py): Verifies and extracts Apple CgBI-compressed PNG files.
- [`tools/printf_check.py`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/tools/printf_check.py): Validates variadic string format specifiers and alignment.
- [`tools/stage_data.sh`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/tools/stage_data.sh): Packages and organizes game assets into `ux0_data/<slug>/`.
- [`tools/deploy_data.py`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/tools/deploy_data.py): Incremental FTP deployment directly to the PS Vita (VitaShell).

---

## LiveArea Assets (`extras/livearea/`)

The repository stores all LiveArea presentation assets in [`extras/livearea/`](file:///Volumes/TOSHIBA%20EXT/PSVITA%20Develop/ios-loader-boilerplate/extras/livearea):
- `icon0.png`: 128×128 application icon (official iOS wordmark on light gray `#E5E5EA`).
- `pic0.png`: 960×544 splash screen ("iPhone OS" in Helvetica Neue typography on charcoal `#202020`).
- `startup.png`: 280×158 LiveArea startup gate button.
- `bg0.png`: 840×500 LiveArea background (faceted geometric low-poly mesh in Apple slate gray).
- `template.xml`: Vita LiveArea layout definition.

These files are automatically mapped directly into `sce_sys/` inside the output `.vpk` by `vita_create_vpk` in `CMakeLists.txt`.

---

## Target Console Directory Layout (`ux0:data/<slug>/`)

```
ux0:data/<slug>/
  <AppName>.app/   Extracted iOS game bundle (.png, .caf, .fsb, plist assets)
  rebase.bin       Rebase relocation table generated by tools/macho_prep.py
  saves/           Game save directory (mapped to iOS Documents/ folder)
  logs/            Runtime execution logs (log_001.log .. log_999.log)
```

---

## Quick Start & Port Initialization

Using `psvita-toolkit`:
```bash
# Initialize a new iOS port directly from an IPA file:
psvita-toolkit init-port --ipa /path/to/game.ipa

# Validate LiveArea assets:
psvita-toolkit livearea validate

# Build vitaGL with SoftFP ABI support:
cd vendor/vitaGL && ./build_vitagl.sh

# Configure and compile the VPK:
cmake -B build
cmake --build build
```
