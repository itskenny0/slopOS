/* 64-bit Linux entry point for the Doom host test: no libc, raw syscalls. */
	.text
	.globl _start
_start:
	pop %rdi			# argc
	mov %rsp, %rsi		# argv
	mov %rdi, %rbx		# save across stub_init (callee-saved)
	mov %rsi, %r12
	and $~15, %rsp		# 16-byte align for calls
	call stub_init
	mov %rbx, %rdi
	mov %r12, %rsi
	call app_main
	mov %eax, %edi
	mov $231, %eax		# exit_group(code)
	syscall
	hlt
