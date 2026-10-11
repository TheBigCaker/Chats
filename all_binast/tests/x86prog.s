/* tests/x86prog.s -- a known x86-64 program, assembled by llvm-mc, that the catalog must
 * decode completely. Every instruction here exercises something the hand-written 73-row
 * catalog could not express: extended registers (REX.R/REX.B), a 32-bit destination
 * zero-extend, a 64-bit immediate, and a shift by an immediate.
 */
.globl _start
.text
_start:
  addq %r8, %r9
  movq %rax, %rbx
  movzbl %al, %ecx
  setg %dl
  leaq (%rax), %rdx
  sarq $3, %rax
  movabsq $0x1122334455667788, %r10
  xorl %eax, %eax
