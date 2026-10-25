.global _start
.type _start, %function
_start:
	ldr x0, [sp]
	add x1, sp, #8
	bl main
	mov x8, #94
	svc #0
