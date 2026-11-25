; asm/test.asm — forms beyond what the runtime uses, assembled by asm/asm.nihil and by nasm,
; which must give the same bytes (make asm-check). Each line is one case.
bits 64
default abs
org 0x401000
start:
    mov rax, rbx
    mov r8, r15
    mov eax, r9d
    mov sil, dil
    mov spl, al
    mov r10b, r11b
    mov ah, bl
    mov al, [rbp]
    mov rax, [r13]
    mov rax, [rsp]
    mov rax, [r12]
    mov rax, [r12 + 8]
    mov rax, [rbp - 8]
    mov rax, [r13 + 127]
    mov rax, [r13 + 128]
    mov rax, [rbx - 128]
    mov rax, [rbx - 129]
    mov rax, [rax + rbx*4 + 16]
    mov rax, [r8 + r9*8 - 4]
    mov rax, [rbx*8 + 64]
    mov rax, [rsi + rsp]
    mov rax, [rcx + 8 + rsi]
    mov rax, [rsi + 8 + rcx - 1]
    mov rax, [rbx + 300 + rsi - 1]
    mov rax, [0x40001000]
    mov [rax], r8b
    mov [r9], si
    mov word [rax], 0x1234
    mov word [rax], -1
    mov dword [rax + 4], 0xFFFFFFFF
    mov qword [rax], -1
    mov qword [rax], 0x7FFFFFFF
    mov byte [rax], 255
    mov byte [rax], -128
    mov ax, 0x1234
    mov r9w, 7
    mov eax, 0xFFFFFFFF
    mov rax, 0xFFFFFFFF
    mov rax, -2147483648
    mov rax, 0x100000000
    mov r12, 0x123456789ABCDEF0
    mov rax, start
    mov r11, start + 8
    mov eax, start
    add rax, 127
    add rax, 128
    add rax, -128
    add rax, -129
    add eax, 0xFFFFFFF0
    add al, 5
    add bl, 5
    add sil, 5
    add ax, 300
    add bx, 3
    add r15w, 0x1000
    sub rax, 0x7FFFFFFF
    sub r8, rcx
    sub rcx, [rdx + 8]
    sub [rdx + 8], rcx
    and r9d, 0xFF
    and rax, -16
    or byte [rbx], 0x80
    xor r10d, r10d
    cmp al, '"'
    cmp byte [rax], ' '
    cmp qword [rbx + 16], 1000
    cmp dword [rbx], -1
    test al, 1
    test r8b, 1
    test eax, 0x100
    test rax, 0x100
    test rbx, rcx
    test byte [rax], 4
    test dword [rax], 0x10000
    lea rax, [rip_target]
    lea rsi, [rel rip_target]
    lea r8, [rax + rbx*2]
    lea rax, [r9*2 - 9]
    lea eax, [rbx + 1]
    movzx eax, byte [rsi]
    movzx r8d, sil
    movzx eax, word [rsi]
    movsx rax, byte [rsi]
    push rax
    push r12
    push 1
    push -1
    push 0x1000
    push qword [rbx]
    pop r15
    pop qword [rbx]
    inc rax
    inc r8d
    dec byte [rax]
    inc qword [rbx + 8]
    neg r9
    not eax
    mul rcx
    div r8
    idiv qword [rax]
    imul rax, rbx
    imul r9, [rax]
    imul rax, rbx, 10
    imul rax, rbx, 1000
    imul eax, ecx, -2
    shl rax, 1
    shl rax, 4
    shr r9d, 31
    sar rax, cl
    shl byte [rax], 2
    rol eax, 3
    bt rax, 5
    bts r8, 63
    btr qword [rax], 2
    bt rax, rcx
    bsr rax, rbx
    bsf r9, [rax]
    bswap eax
    bswap r9d
    bswap rax
    xchg al, ah
    xchg rax, rcx
    xchg rcx, rax
    xchg eax, r9d
    xchg rbx, rdx
    xchg r8b, r9b
    sete al
    setne r8b
    setl byte [rax]
    cqo
    syscall
    nop
    cvtsi2sd xmm0, rax
    cvtsi2sd xmm9, r10
    divsd xmm1, [rax]
    mulsd xmm0, [rel rip_target]
    movq rax, xmm0
    movq xmm3, r9
    movq xmm0, [rax]
    movq [rax], xmm0
    movq xmm10, [r9 + 8]
    movq [rbx + rcx*8], xmm12
    prefetcht0 [rax + 64]
    rep movsb
    rep movsq
    rep stosq
    repe cmpsb
    repne scasb
    jmp short near_label
    jmp near near_label
    jz near_label
    jnz far_label
    jmp far_label
    call far_label
    jmp rax
    jmp [rax + 8]
    jmp qword [rbx]
    call r9
    jmp strict near near_label
    ; values from labels: never the 8-bit forms, mov r64 takes 64 bits (as nasm)
    add rax, start
    cmp rax, start
    push start
    mov rax, [rbx + start]
    mov rax, -start
    mov rax, near_label - start + 3
    mov rax, start*0
    mov rax, strict qword 1
    add rax, strict dword 1
near_label:
    ret
rip_target:
    dq 1.0e0, 2.5e1, 1.0e22
    dd 0x12345678, -1
    dw 0xABCD
    db "text", 0, 'a', 10
    times 4 db 0x90
    align 16
    times 200 db 0
far_label:
    dw far_label - near_label
s_len equ $ - rip_target
    dq s_len
