    .syntax unified
    .arm
    .text

@ Import sin implementar: el thunk deja su índice en r12. Registra el nombre y devuelve 0
@ (r0 = r1 = 0, que sirve como "nil" para ObjC y 0 para retornos de 64 bits).
    .global ml_unimpl_entry
    .type ml_unimpl_entry, %function
ml_unimpl_entry:
    push    {r4, lr}
    mov     r4, sp
    bic     sp, sp, #7          @ ml_unimpl_log es código de la PS Vita
    mov     r0, r12
    blx     ml_unimpl_log
    mov     sp, r4
    pop     {r4, lr}
    mov     r0, #0
    mov     r1, #0
    bx      lr

@ uint32_t call_guest(void *fn, a0, a1, a2, a3): llama al juego guardando r9, que el código
@ iOS trata como registro temporal.
    .global call_guest
    .type call_guest, %function
call_guest:
    push    {r4, r9, r10, lr}
    mov     r12, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    ldr     r3, [sp, #16]
    blx     r12
    pop     {r4, r9, r10, pc}

@ uint32_t call_guest6(void *fn, a0, a1, a2, a3, a4, a5): igual que call_guest, con 2 argumentos
@ más por pila (render callback de AudioUnit / funciones de 6 argumentos).
    .global call_guest6
    .type call_guest6, %function
call_guest6:
    push    {r4, r5, r9, r10, r11, lr}
    mov     r12, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    ldr     r3, [sp, #24]
    ldr     r4, [sp, #28]
    ldr     r5, [sp, #32]
    sub     sp, sp, #8
    str     r4, [sp]
    str     r5, [sp, #4]
    blx     r12
    add     sp, sp, #8
    pop     {r4, r5, r9, r10, r11, pc}

@ Entrada a toda función nuestra llamada por el juego (imports y hooks), con ip = destino.
@ El ABI de iOS ARMv6 solo garantiza pila alineada a 4; el código de la Vita (newlib, vitaGL)
@ asume 8 (AAPCS) y falla con pila desalineada (NEON con alineación, va_arg de 64 bits). Si sp
@ está desalineado, se arma un marco alineado copiando 16 palabras de argumentos de pila.
    .global ml_align_call
    .type ml_align_call, %function
ml_align_call:
    tst     sp, #4
    bxeq    ip
    push    {r4-r8, r10, r11, lr}
    mov     r4, sp
    sub     sp, sp, #64
    bic     sp, sp, #7
    add     r5, r4, #32
    mov     lr, sp
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    blx     ip
    mov     sp, r4
    pop     {r4-r8, r10, r11, pc}
