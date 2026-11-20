/* SPDX-License-Identifier: Apache-2.0
 *
 * Adapts TinySoundFont (MIT, see vendor/LICENSE.tsf) to the VIBE FM rules:
 * no heap, no libc maths, no stdio. TSF's allocation and math macros are
 * redirected to a static arena and the SDK's vd_* helpers. No upstream file
 * is modified; this translation unit is the only place with TSF_IMPLEMENTATION.
 */
#include "tsf_config.h"
#include "tsf_port.h"

#define SF_ARENA_BYTES (16u * 1024u * 1024u)

static uint8_t arena[2][SF_ARENA_BYTES] __attribute__((aligned(16)));
static uint8_t *cur_base;
static uint8_t *cur_ptr;
static size_t cur_cap;

void sf_arena_select(int bank)
{
  if (bank == 0 || bank == 1) {
    cur_base = arena[bank];
    cur_ptr = cur_base;
    cur_cap = SF_ARENA_BYTES;
  } else {
    cur_base = 0;
    cur_ptr = 0;
    cur_cap = 0;
  }
}

void sf_arena_reset(int bank)
{
  sf_arena_select(bank);
}

int sf_arena_used(int bank)
{
  if ((bank == 0 || bank == 1) && cur_base == arena[bank]) return (int)(cur_ptr - cur_base);
  return 0;
}

/* A 16-byte header before each allocation remembers its size, so realloc can
   copy only the bytes that actually belonged to the old block. */
void *sf_tsf_alloc(unsigned int n)
{
  uint8_t *p;
  unsigned int payload;
  if (!cur_base) return 0;
  if (n == 0) n = 1;
  p = cur_ptr;
  payload = (n + 15u) & ~15u;
  if (payload < n || (size_t)payload + 16u > cur_cap - (size_t)(p - cur_base)) return 0;
  ((unsigned int *)p)[0] = n;
  cur_ptr = p + 16 + payload;
  return p + 16;
}

void sf_tsf_free(void *p)
{
  (void)p; /* arenas are reset as a whole before loading the next font */
}

void *sf_tsf_realloc(void *p, unsigned int n)
{
  void *q;
  unsigned int old, copy;
  if (!p) return sf_tsf_alloc(n);
  q = sf_tsf_alloc(n);
  if (!q) return 0;
  old = ((unsigned int *)p)[-1];
  copy = old < n ? old : n;
  memcpy(q, p, copy);
  return q;
}

#define TSF_IMPLEMENTATION
#include "vendor/tsf.h"
