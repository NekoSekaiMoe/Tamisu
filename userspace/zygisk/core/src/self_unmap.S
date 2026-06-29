/* SPDX-License-Identifier: GPL-3.0 */
/*
 * Tamisu - synchronous self-unmap for denylist mode-1 (AArch64).
 *
 * After cleanup, restore the specialize hook's entry frame (x30 = ART return
 * addr, sp, x19..x29) and tail-call munmap: munmap runs in libc, drops the whole
 * core image, and its `ret` lands straight in ART. The core call stack is
 * discarded atomically -- no core instruction runs after the unmap, so there is
 * no dangling-PC crash (an async munmap firing while core was still mid-libc-call
 * did exactly that).
 *
 *  - tamisu_cap_tmpl: capture-stub template; the inline hook copies it per trampoline
 *    page and patches its two literals (&g_tamisu_ret_ctx, wrapper). The patched
 *    native jumps here first, saving the wrapper's pristine entry frame
 *    (x19..x30 + sp; only x16/x17 clobbered) into g_tamisu_ret_ctx, then jumps to the
 *    real wrapper (x30 unchanged, so a normal return still lands in ART).
 *  - tamisu_self_unmap_tail(base, size): restore that frame and `b munmap`.
 *
 * Author: Anatdx
 */

  .bss
  .align 4
  .globl g_tamisu_ret_ctx
  .hidden g_tamisu_ret_ctx            /* hidden: same-.so only -> PC-rel reloc legal */
g_tamisu_ret_ctx:
  .skip 8 * 13            /* x19,x20,x21,x22,x23,x24,x25,x26,x27,x28,x29,x30,sp */

  .text

/* capture stub template -- copied + literal-patched per hook by the inline hook.
 * ldr-literal loads are PC-relative and survive the memcpy unchanged. */
  .globl tamisu_cap_tmpl
  .hidden tamisu_cap_tmpl
  .globl tamisu_cap_tmpl_ctx
  .hidden tamisu_cap_tmpl_ctx
  .globl tamisu_cap_tmpl_wrap
  .hidden tamisu_cap_tmpl_wrap
  .globl tamisu_cap_tmpl_end
  .hidden tamisu_cap_tmpl_end
  .align 4
tamisu_cap_tmpl:
  ldr   x16, tamisu_cap_tmpl_ctx     /* x16 = &g_tamisu_ret_ctx (patched literal) */
  stp   x19, x20, [x16, #0]
  stp   x21, x22, [x16, #16]
  stp   x23, x24, [x16, #32]
  stp   x25, x26, [x16, #48]
  stp   x27, x28, [x16, #64]
  stp   x29, x30, [x16, #80]     /* x29=fp, x30=lr (ART return address) */
  mov   x17, sp
  str   x17, [x16, #96]          /* sp at the wrapper's entry (= ART's sp) */
  ldr   x16, tamisu_cap_tmpl_wrap    /* x16 = real wrapper (patched literal) */
  br    x16                      /* jump wrapper; x30 still = ART return addr */
  .align 3
tamisu_cap_tmpl_ctx:
  .quad 0                        /* inline hook patches -> &g_tamisu_ret_ctx */
tamisu_cap_tmpl_wrap:
  .quad 0                        /* inline hook patches -> wrapper */
tamisu_cap_tmpl_end:

/* [[noreturn]] void tamisu_self_unmap_tail(void *base, size_t size)
 * x0=base, x1=size already set for munmap. Restore the captured entry frame and
 * tail-call munmap; munmap's ret lands at the saved x30 (ART). */
  .globl tamisu_self_unmap_tail
  .hidden tamisu_self_unmap_tail
  .type  tamisu_self_unmap_tail, %function
  .align 4
tamisu_self_unmap_tail:
  adrp  x16, g_tamisu_ret_ctx
  add   x16, x16, :lo12:g_tamisu_ret_ctx
  ldp   x19, x20, [x16, #0]
  ldp   x21, x22, [x16, #16]
  ldp   x23, x24, [x16, #32]
  ldp   x25, x26, [x16, #48]
  ldp   x27, x28, [x16, #64]
  ldp   x29, x30, [x16, #80]     /* x30 = ART return address */
  ldr   x17, [x16, #96]
  mov   sp, x17                  /* sp = wrapper entry sp */
  b     munmap                   /* munmap(base,size); ret -> x30 (ART) */
  .size tamisu_self_unmap_tail, .-tamisu_self_unmap_tail
