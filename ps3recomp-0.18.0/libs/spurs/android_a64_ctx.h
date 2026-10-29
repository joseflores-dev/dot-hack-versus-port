/* Android/aarch64 stand-in for the ucontext routines cellFiber.c uses.
 *
 * Bionic declares ucontext_t but implements none of getcontext, makecontext, swapcontext or setcontext, so this
 * header supplies the four over a context that holds only what AAPCS64 makes a callee preserve: x19-x30, sp and
 * d8-d15. Included by cellFiber.c alone; the macros below rename its calls and its ucontext_t to these.
 */
#ifndef PS3RECOMP_ANDROID_A64_CTX_H
#define PS3RECOMP_ANDROID_A64_CTX_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

typedef struct a64_ctx {
    uint64_t regs[21];           /* 0: x19-x28, 80: x29, 88: x30, 96: sp, 104: d8-d15 */
    struct { void* ss_sp; size_t ss_size; } uc_stack;
    struct a64_ctx* uc_link;     /* unused: a fiber never returns from its entry */
    void (*fn)(unsigned, unsigned);
    unsigned arg[2];
} a64_ctx;

/* Saves the callee-saved state into `from`, then resumes `to`. Returns 0 when `from` is resumed. */
int a64_swapcontext(a64_ctx* from, const a64_ctx* to);
/* Resumes `to`; never returns. */
int a64_setcontext(const a64_ctx* to);

/* A made context starts here with x19 = its a64_ctx. */
void a64_fiber_start(a64_ctx* c)
{
    c->fn(c->arg[0], c->arg[1]);
    abort();                     /* the entry ended without switching away */
}

__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl a64_swapcontext\n"
    ".type a64_swapcontext, %function\n"
    "a64_swapcontext:\n"
    "    stp x19, x20, [x0, #0]\n"
    "    stp x21, x22, [x0, #16]\n"
    "    stp x23, x24, [x0, #32]\n"
    "    stp x25, x26, [x0, #48]\n"
    "    stp x27, x28, [x0, #64]\n"
    "    stp x29, x30, [x0, #80]\n"
    "    mov x9, sp\n"
    "    str x9, [x0, #96]\n"
    "    stp d8, d9, [x0, #104]\n"
    "    stp d10, d11, [x0, #120]\n"
    "    stp d12, d13, [x0, #136]\n"
    "    stp d14, d15, [x0, #152]\n"
    "    mov x0, x1\n"
    "    b a64_setcontext\n"
    ".size a64_swapcontext, .-a64_swapcontext\n"
    ".globl a64_setcontext\n"
    ".type a64_setcontext, %function\n"
    "a64_setcontext:\n"
    "    ldp x19, x20, [x0, #0]\n"
    "    ldp x21, x22, [x0, #16]\n"
    "    ldp x23, x24, [x0, #32]\n"
    "    ldp x25, x26, [x0, #48]\n"
    "    ldp x27, x28, [x0, #64]\n"
    "    ldp x29, x30, [x0, #80]\n"
    "    ldr x9, [x0, #96]\n"
    "    mov sp, x9\n"
    "    ldp d8, d9, [x0, #104]\n"
    "    ldp d10, d11, [x0, #120]\n"
    "    ldp d12, d13, [x0, #136]\n"
    "    ldp d14, d15, [x0, #152]\n"
    "    mov x0, #0\n"
    "    ret\n"
    ".size a64_setcontext, .-a64_setcontext\n"
    ".type a64_trampoline, %function\n"
    "a64_trampoline:\n"
    "    mov x0, x19\n"
    "    bl a64_fiber_start\n"
    "    brk #0\n"
    ".size a64_trampoline, .-a64_trampoline\n");

void a64_trampoline(void);

static inline int a64_getcontext(a64_ctx* c)
{
    (void)c;                     /* makecontext fills in everything a made context needs */
    return 0;
}

/* Only the (fn, 2, hi, lo) form cellFiber.c uses is supported. */
static inline void a64_makecontext(a64_ctx* c, void (*fn)(void), int argc, ...)
{
    va_list ap;
    va_start(ap, argc);
    c->arg[0] = argc > 0 ? va_arg(ap, unsigned) : 0;
    c->arg[1] = argc > 1 ? va_arg(ap, unsigned) : 0;
    va_end(ap);
    c->fn = (void (*)(unsigned, unsigned))fn;
    for (int i = 0; i < 21; i++) c->regs[i] = 0;
    c->regs[0]  = (uint64_t)(uintptr_t)c;                                    /* x19 */
    c->regs[11] = (uint64_t)(uintptr_t)a64_trampoline;                       /* x30 */
    c->regs[12] = ((uint64_t)(uintptr_t)c->uc_stack.ss_sp + c->uc_stack.ss_size) & ~(uint64_t)15; /* sp */
}

#define ucontext_t  a64_ctx
#define getcontext  a64_getcontext
#define makecontext a64_makecontext
#define swapcontext a64_swapcontext
#define setcontext  a64_setcontext

#endif
