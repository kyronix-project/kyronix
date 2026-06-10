bits 64
section .data
msg db "Hello from nasm on Kyronix!", 10
msglen equ $ - msg

section .text
global _start
_start:
    mov rax, 1
    mov rdi, 1
    mov rsi, msg
    mov rdx, msglen
    syscall
    mov rax, 60
    xor rdi, rdi
    syscall
