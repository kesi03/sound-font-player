/* SPDX-License-Identifier: Apache-2.0
 *
 * Shared TinySoundFont configuration. Every macro that could make tsf.h pull
 * in libc is defined here; both translation units include this before tsf.h.
 * The implementation lives in tsf_port.c, the declarations in main.c only.
 */
#ifndef SF2_TSF_CONFIG_H
#define SF2_TSF_CONFIG_H

#include "vibe.h"
#include "vibe_dsp.h"

/* The SDK provides these weak freestanding symbols. */
extern void *memcpy(void *d, const void *s, size_t n);
extern void *memset(void *d, int c, size_t n);

/* Arena allocator implemented by tsf_port.c. */
void *sf_tsf_alloc(unsigned int n);
void *sf_tsf_realloc(void *p, unsigned int n);
void  sf_tsf_free(void *p);

/* Freestanding maths built on the SDK's vd_* helpers. */
static double sf_tsf_pow(double a, double b) { return (double)vd_pow((float)a, (float)b); }
static float  sf_tsf_powf(float a, float b) { return vd_pow(a, b); }
static float  sf_tsf_expf(float x) { return vd_exp2(x * 1.4426950408889634f); }
static double sf_tsf_log(double x) { return x > 0.0 ? (double)(vd_log2((float)x) * 0.6931471805599453f) : -100.0; }
static float  sf_tsf_log10(float x) { return x > 0.0f ? vd_log2(x) * 0.3010299956639812f : -100.0f; }
static double sf_tsf_sqrt(double x) { return (double)vd_sqrt(x > 0.0 ? (float)x : 0.0f); }
static float  sf_tsf_sqrtf(float x) { return vd_sqrt(x < 0.0f ? 0.0f : x); }
/* TSF_PI * Fc arrives in radians; vd_sin takes turns (0..1 = 2*pi). */
static double sf_tsf_tan(double radians)
{
  float phase = (float)radians * 0.15915494309189535f;
  float s = vd_sin(phase);
  float c = vd_sin(phase + 0.25f);
  if (c == 0.0f) return 0.0;
  return (double)(s / c);
}

#define TSF_NO_STDIO
#define TSF_MALLOC(n)    sf_tsf_alloc((unsigned int)(n))
#define TSF_REALLOC(p,n) sf_tsf_realloc((void *)(p), (unsigned int)(n))
#define TSF_FREE(p)      sf_tsf_free((void *)(p))
#define TSF_MEMCPY       memcpy
#define TSF_MEMSET       memset
#define TSF_POW          sf_tsf_pow
#define TSF_POWF         sf_tsf_powf
#define TSF_EXPF         sf_tsf_expf
#define TSF_LOG          sf_tsf_log
#define TSF_LOG10        sf_tsf_log10
#define TSF_SQRT         sf_tsf_sqrt
#define TSF_SQRTF        sf_tsf_sqrtf
#define TSF_TAN          sf_tsf_tan

#endif
