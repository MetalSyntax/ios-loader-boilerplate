# iOS Mach-O Loader Boilerplate para PS Vita

Boilerplate y scaffold reutilizable para portar juegos clásicos de iOS (ARMv6 / ARMv7, 32-bit Mach-O `MH_EXECUTE`) a PlayStation Vita.
Equivalente para iOS de `soloader-boilerplate` (Android). Basado en la arquitectura probada y funcional desarrollada en `Inotia2-vita`.

## Arquitectura del Loader

| Componente | Archivo(s) | Qué reemplaza / funcionalidad |
|---|---|---|
| **Mach-O Loader** | `source/macho.c`, `source/macho.h` | Dyld y cargador de ejecutables Mach-O de 32 bits a memoria fija (`MACHO_SLIDE = 0x98000000`). Aplica rebase de datos, resuelve tabla de imports (`__la_symbol_ptr`, `__nl_symbol_ptr`) y asigna thunks para imports no implementados. |
| **Assembly Glue & Trampolines** | `source/asm.S` | `call_guest` y `call_guest6` (preservan `r9`). `ml_align_call` (realinea la pila de 4 bytes de iOS a 8 bytes AAPCS de VitaSDK antes de entrar a newlib/vitaGL). `ml_unimpl_entry` (thunk con logging automático). |
| **Darwin / POSIX Subsystem** | `source/darwin.c`, `source/darwin.h` | Emulación libc/libgcc/pthreads/sockets de Darwin. Pthread seguro con manejo de `SceUID`s (previene crash en `pthread_detach`). `mach_absolute_time()` vía `sceKernelGetProcessTimeWide()`. |
| **Variadic Formatter** | `source/darwin_printf.c`, `source/darwin_printf.h` | Formateador printf compatible con el ABI de Darwin: dobles y enteros de 64 bits en palabras no alineadas, y soporte para `%@` de Objective-C (`NSString*`). |
| **Mini-runtime ObjC / UIKit** | `source/objc.c`, `source/objc.h` | Dispatch por selectores con `objc_msgSend` y `objc_msgSend_stret`. Soporte para `NSString`, `UIFont` (renderizado sobre atlas con FreeType), `NSBundle` (`resourcePath`), `NSCalendar`, `UIDevice`, y teclado mediante `sceImeDialog`. |
| **Render GL (OpenGL ES)** | `source/gles.c`, `source/gles.h`, `source/vitagl_no_splash.c` | Backend GLES1 / GLES2 sobre `vitaGL` vendorizada con `SOFTFP_ABI=1`. FBO virtual para soportar rotación y escalado de pantalla (portrait/landscape a 960x544). Conversión de arrays `GL_FIXED`. Desactivación de splashscreen que congela frames. |
| **CgBI PNG Decoder** | `source/cgbi.c`, `source/cgbi.h` | Descompresor y reversor de byte-swap para imágenes PNG con optimización CgBI de Apple, permitiendo leer los recursos del IPA intactos sin convertirlos en disco. |
| **CoreAudio Backend** | `source/coreaudio.c` | AudioUnit RemoteIO + AudioSession emulados sobre un hilo dedicado de `sceAudioOut`. |
| **Input** | `source/input.c`, `source/input.h` | Multitouch frontal de la Vita y botones físicos mapeados a eventos táctiles o botones virtuales. |
| **Logging** | `source/log.c`, `source/log.h` | Logging rotativo en la consola (`ux0:data/<slug>/logs/log_001.log` .. `log_999.log`). |
| **Game Patches & Glue** | `source/patch.c`, `source/patch.h` | Ganchos dedicados para implementar la lógica específica del juego a portar. |

## Reglas Críticas de ABI de iOS en PS Vita

1. **`r9` como registro temporal**: En el ABI de iOS ARMv6/v7, `r9` no es callee-saved. Todo el código del loader se compila con `-ffixed-r9` y cualquier salto hacia el binario pasa por `call_guest` para no corromper el registro.
2. **Convención SoftFP**: Los flotantes y dobles viajan en registros enteros (`r0..r3`). vitaGL DEBE compilarse con `SOFTFP_ABI=1` para que las llamadas a GXM (`sceGxmSetViewport`, etc.) no destruyan la matriz de proyección.
3. **Alineación de pila a 4 bytes**: El código huésped de iOS asume alineación de pila a 4 bytes, mientras que el SDK de Vita y newlib esperan 8 bytes (AAPCS). Todas las funciones importadas llamadas desde el binario del juego entran por `ml_align_call`.
4. **Cifrado FairPlay DRM**: El Mach-O original DEBE estar descifrado (`cryptid = 0`). Si `cryptid != 0`, el juego crasheará inmediatamente con Data Abort.

## Herramientas Incluidas (`tools/`)

- `macho_prep.py`: Analiza el binario (thin o fat universal), valida punteros en `__DATA` contra la tabla de símbolos y genera `rebase.bin`.
- `annotate.py`: Desensambla funciones cruzando llamadas PIC con selectores ObjC, clases y cadenas literales.
- `cgbi_check.py`: Valida texturas PNG CgBI de Apple.
- `stage_data.sh`: Prepara el bundle de datos para la consola en `ux0_data/<slug>/`.
- `deploy_data.py`: Sincronización FTP incremental con la consola (VitaShell).

## Estructura en la Consola (`ux0:data/<slug>/`)

```
ux0:data/<slug>/
  <AppName>.app/   bundle del juego (ejecutable y recursos .png/.caf/.fsb)
  rebase.bin       tabla de rebase generada por tools/macho_prep.py
  saves/           partidas guardadas (equivalente al Documents/ de iOS)
  logs/            log_001.log .. log_999.log
```
