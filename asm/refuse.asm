; asm/refuse.asm — what asm/asm.nihil must refuse. Each case starts with "; --- expect: TEXT",
; the text its message must hold; make asm-check assembles each one alone (after "bits 64").
; --- expect: ah, bh, ch or dh
mov ah, r8b
; --- expect: ah, bh, ch or dh
mov ah, sil
; --- expect: different sizes
mov eax, rcx
; --- expect: different sizes
mov byte [rax], ecx
; --- expect: the size is not known
mov [rax], 1
; --- expect: does not fit in a 32-bit
mov qword [rax], 0x100000000
; --- expect: does not fit in a 32-bit
test rax, 0x100000000
; --- expect: does not fit in a 32-bit
add rax, 0x80000000
; --- expect: does not fit in a 32-bit
imul rax, rbx, 0x100000000
; --- expect: does not fit in a 32-bit
push 0x100000000
; --- expect: a displacement past 32 bits
mov eax, [rbx + 0x100000000]
; --- expect: [rel …] with a register
mov eax, [rel rbx + 8]
; --- expect: rsp cannot be an index
mov eax, [rsp*4]
; --- expect: one factor only
mov eax, [rax*2*4]
; --- expect: pop takes
pop 1
; --- expect: ret takes 0
ret 16
; --- expect: 16-bit operands
test ax, 1
; --- expect: 16-bit operands
lea ax, [rbx]
; --- expect: 16-bit operands
push ax
; --- expect: 16-bit operands
movzx ax, bl
; --- expect: default abs only
default rel
; --- expect: defined twice
L:
L:
; --- expect: %define with parameters
%define F(x) x
nop
; --- expect: without %endmacro
%macro M 0
nop
; --- expect: a short jump whose target is too far
jmp short L
times 200 db 0
L:
; --- expect: does not fit in 1 byte
mov byte [rax], 256
; --- expect: does not fit in 1 byte
shl rax, 256
; --- expect: not defined
db undefined_label
; --- expect: not known: frobnicate
frobnicate rax
; --- expect: inc takes 1
inc rax, rbx
; --- expect: movq here
movq xmm0, xmm1
; --- expect: takes a 64-bit register
push eax
; --- expect: goes from 8 or 16 bits
movzx rax, rcx
; --- expect: labels can only be added or subtracted
L:
mov rax, L & 15
; --- expect: labels can only be added or subtracted
L:
mov rax, L*2
; --- expect: does not fit in 1 byte
db 300
; --- expect: takes a 64-bit register or qword memory
push dword [rax]
; --- expect: takes a 64-bit register or qword memory
pop dword [rax]
