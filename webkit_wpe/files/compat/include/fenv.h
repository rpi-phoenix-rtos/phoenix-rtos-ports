/*
 * Phoenix-RTOS browser track C (JavaScriptCore) -- local compat, not part of libphoenix.
 *
 * <fenv.h> for AArch64. TODO(browser-B1): libphoenix ships libmcs's <fenv.h>, which is an
 * `#error` stub ("shall not be used as is"), so any C++ file that includes <cfenv>/<fenv.h> (WTF's
 * bundled SIMDe does) cannot compile. This is a complete C99 implementation over the AArch64
 * FPCR/FPSR registers, header-only (static inline), so it needs no library and cannot clash with
 * libm's stub symbols. Delete it once libphoenix has a real <fenv.h>.
 *
 * Trapping is never enabled (FPCR trap-enable bits stay 0, as on Linux), so feraiseexcept()
 * only sets the sticky FPSR flags.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PHOENIX_JSC_COMPAT_FENV_H
#define PHOENIX_JSC_COMPAT_FENV_H

#if !defined(__aarch64__)
#error "compat <fenv.h> is AArch64-only"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	unsigned int __fpcr;
	unsigned int __fpsr;
} fenv_t;

typedef unsigned int fexcept_t;

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x02
#define FE_OVERFLOW   0x04
#define FE_UNDERFLOW  0x08
#define FE_INEXACT    0x10
#define FE_ALL_EXCEPT 0x1f

#define FE_TONEAREST  0x000000
#define FE_UPWARD     0x400000
#define FE_DOWNWARD   0x800000
#define FE_TOWARDZERO 0xc00000

#define __PHX_FE_RMODE_MASK 0xc00000u

#define FE_DFL_ENV ((const fenv_t *)-1)


static inline unsigned int __phx_get_fpcr(void)
{
	unsigned long v;
	__asm__ volatile("mrs %0, fpcr" : "=r"(v));
	return (unsigned int)v;
}


static inline void __phx_set_fpcr(unsigned int v)
{
	__asm__ volatile("msr fpcr, %0" : : "r"((unsigned long)v));
}


static inline unsigned int __phx_get_fpsr(void)
{
	unsigned long v;
	__asm__ volatile("mrs %0, fpsr" : "=r"(v));
	return (unsigned int)v;
}


static inline void __phx_set_fpsr(unsigned int v)
{
	__asm__ volatile("msr fpsr, %0" : : "r"((unsigned long)v));
}


static inline int feclearexcept(int excepts)
{
	__phx_set_fpsr(__phx_get_fpsr() & ~((unsigned int)excepts & FE_ALL_EXCEPT));
	return 0;
}


static inline int fegetexceptflag(fexcept_t *flagp, int excepts)
{
	*flagp = __phx_get_fpsr() & (unsigned int)excepts & FE_ALL_EXCEPT;
	return 0;
}


static inline int feraiseexcept(int excepts)
{
	__phx_set_fpsr(__phx_get_fpsr() | ((unsigned int)excepts & FE_ALL_EXCEPT));
	return 0;
}


static inline int fesetexceptflag(const fexcept_t *flagp, int excepts)
{
	unsigned int mask = (unsigned int)excepts & FE_ALL_EXCEPT;
	__phx_set_fpsr((__phx_get_fpsr() & ~mask) | (*flagp & mask));
	return 0;
}


static inline int fetestexcept(int excepts)
{
	return (int)(__phx_get_fpsr() & (unsigned int)excepts & FE_ALL_EXCEPT);
}


static inline int fegetround(void)
{
	return (int)(__phx_get_fpcr() & __PHX_FE_RMODE_MASK);
}


static inline int fesetround(int round)
{
	if (((unsigned int)round & ~__PHX_FE_RMODE_MASK) != 0u) {
		return -1;
	}
	__phx_set_fpcr((__phx_get_fpcr() & ~__PHX_FE_RMODE_MASK) | (unsigned int)round);
	return 0;
}


static inline int fegetenv(fenv_t *envp)
{
	envp->__fpcr = __phx_get_fpcr();
	envp->__fpsr = __phx_get_fpsr();
	return 0;
}


static inline int feholdexcept(fenv_t *envp)
{
	fegetenv(envp);
	__phx_set_fpsr(envp->__fpsr & ~(unsigned int)FE_ALL_EXCEPT);
	return 0;
}


static inline int fesetenv(const fenv_t *envp)
{
	if (envp == FE_DFL_ENV) {
		__phx_set_fpcr(__phx_get_fpcr() & ~__PHX_FE_RMODE_MASK);
		__phx_set_fpsr(__phx_get_fpsr() & ~(unsigned int)FE_ALL_EXCEPT);
	}
	else {
		__phx_set_fpcr(envp->__fpcr);
		__phx_set_fpsr(envp->__fpsr);
	}
	return 0;
}


static inline int feupdateenv(const fenv_t *envp)
{
	unsigned int raised = __phx_get_fpsr() & FE_ALL_EXCEPT;
	fesetenv(envp);
	feraiseexcept((int)raised);
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif
