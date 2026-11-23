; TW3SE core - engine functions hooked from Lua (w3.engine_hook; the Lua side is in lua.cpp).
;
; One stub per slot. It is entered in place of the engine function (MinHook's jump), with the function's arguments
; where the caller put them: rcx, rdx, r8, r9, xmm0-xmm3, the rest on the stack from entry rsp+28h. The stub keeps the
; four registers and the four float registers, calls tw3se_enghook_pre(slot, regs, stack) (which may rewrite the
; four registers), puts them back and jumps to the original. The return address is never touched, so a fault or an
; exception unwinds through the original as before; the stub's own frame has unwind data (PROC FRAME).
;
; Frame: entry rsp is 8 mod 16, 0A8h more keeps the call aligned. [rsp+0..1F] shadow space, [rsp+20h..3Fh] the four
; registers, [rsp+40h..7Fh] xmm0-xmm3. The caller's fifth argument lies at entry rsp+28h = rsp+0D0h.

EXTERN tw3se_enghook_pre:PROC
EXTERN g_enghookOrig:QWORD

.code

ENGTHUNK MACRO n
enghook_thunk_&n& PROC FRAME
    sub rsp, 0A8h
    .allocstack 0A8h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    movdqu xmmword ptr [rsp+40h], xmm0
    movdqu xmmword ptr [rsp+50h], xmm1
    movdqu xmmword ptr [rsp+60h], xmm2
    movdqu xmmword ptr [rsp+70h], xmm3
    mov ecx, n
    lea rdx, [rsp+20h]
    lea r8, [rsp+0D0h]
    call tw3se_enghook_pre
    mov rcx, [rsp+20h]
    mov rdx, [rsp+28h]
    mov r8, [rsp+30h]
    mov r9, [rsp+38h]
    movdqu xmm0, xmmword ptr [rsp+40h]
    movdqu xmm1, xmmword ptr [rsp+50h]
    movdqu xmm2, xmmword ptr [rsp+60h]
    movdqu xmm3, xmmword ptr [rsp+70h]
    add rsp, 0A8h
    lea rax, g_enghookOrig
    jmp qword ptr [rax + n*8]
enghook_thunk_&n& ENDP
ENDM

ENGTHUNK 0
ENGTHUNK 1
ENGTHUNK 2
ENGTHUNK 3
ENGTHUNK 4
ENGTHUNK 5
ENGTHUNK 6
ENGTHUNK 7
ENGTHUNK 8
ENGTHUNK 9
ENGTHUNK 10
ENGTHUNK 11
ENGTHUNK 12
ENGTHUNK 13
ENGTHUNK 14
ENGTHUNK 15

; uint64 tw3se_engcall(void* fn, const uint64* regs, const uint64* stack, uint64 nstack, uint64* xmm0Out)
; Calls an engine function for w3.engine_call (lua.cpp): regs = rcx, rdx, r8, r9, then the bits of xmm0-xmm3 (eight
; qwords; by Windows x64 the k-th argument goes to the k-th integer or float register); stack = the arguments from the
; fifth on. Returns rax; xmm0's low qword goes to *xmm0Out. rbp keeps the frame, so the stack area may have any size.
tw3se_engcall PROC FRAME
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push rbx
    .pushreg rbx
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog
    mov r10, rcx
    mov rsi, rdx
    mov rbx, [rbp+48h]
    mov rax, r9
    shl rax, 3
    add rax, 20h
    sub rsp, rax
    and rsp, -16
    xor ecx, ecx
copy_args:
    cmp rcx, r9
    jae args_done
    mov rdx, [r8+rcx*8]
    mov [rsp+20h+rcx*8], rdx
    inc rcx
    jmp copy_args
args_done:
    mov rcx, [rsi]
    mov rdx, [rsi+8]
    mov r8, [rsi+10h]
    mov r9, [rsi+18h]
    movq xmm0, qword ptr [rsi+20h]
    movq xmm1, qword ptr [rsi+28h]
    movq xmm2, qword ptr [rsi+30h]
    movq xmm3, qword ptr [rsi+38h]
    call r10
    movq qword ptr [rbx], xmm0
    mov rsp, rbp
    pop rbx
    pop rdi
    pop rsi
    pop rbp
    ret
tw3se_engcall ENDP

END
