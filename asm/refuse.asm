; asm/refuse.asm — what asm/asm.nihil must refuse (each case between lines "; ---"): forms it
; does not encode, and mistakes nasm also rejects. make asm-check assembles each one alone.
mov ah, r8b
; ---
mov ah, sil
; ---
mov eax, rcx
; ---
mov byte [rax], ecx
; ---
mov [rax], 1
; ---
mov qword [rax], 0x100000000
; ---
test rax, 0x100000000
; ---
add rax, 0x80000000
; ---
imul rax, rbx, 0x100000000
; ---
push 0x100000000
; ---
mov eax, [rbx + 0x100000000]
; ---
mov eax, [rel rbx + 8]
; ---
mov eax, [rsp*4]
; ---
mov eax, [rax*2*4]
; ---
pop 1
; ---
ret 16
; ---
test ax, 1
; ---
lea ax, [rbx]
; ---
push ax
; ---
movzx ax, bl
; ---
default rel
; ---
L:
L:
; ---
%define F(x) x
nop
; ---
%macro M 0
nop
; ---
jmp short L
times 200 db 0
L:
; ---
mov byte [rax], 256
; ---
shl rax, 256
; ---
db undefined_label
; ---
frobnicate rax
; ---
inc rax, rbx
