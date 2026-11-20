/* SPDX-License-Identifier: Apache-2.0
 *
 * fx.c - master effects for SF2 PLAYER.
 *
 * Chain: drive -> chorus -> ping-pong delay -> stereo reverb -> tone -> tremolo.
 * Every effect is a wet/dry amount (0..1000) published by the control flow as
 * an atomic parameter; all delay-line state below lives in the audio flow only.
 * No heap, no libc, no host calls from the audio callback.
 */
#include "vibe_dsp.h"
#include "fx.h"

#define FX_REF_SR       44100.0f
#define FX_CHORUS_LEN   2048
#define FX_DELAY_MAX    24000
#define FX_REV_COMBS    4
#define FX_REV_APS      2
#define FX_REV_COMB_MAX 2048
#define FX_REV_AP_MAX   768

static float fx_sr = 48000.0f;
static float fx_ratio = 1.0f;

/* --- chorus: two modulated taps per channel -------------------------------- */
static float ch_buf[2][FX_CHORUS_LEN];
static int ch_pos[2];
static float ch_phase;

/* --- delay: cross-fed feedback, one line per channel ----------------------- */
static float dl_buf[2][FX_DELAY_MAX];
static int dl_pos[2], dl_len;
static float dl_lp[2];

/* --- reverb: 4 combs + 2 allpasses per channel, lightly cross-fed ---------- */
static const int rev_comb_base[FX_REV_COMBS] = { 1440, 1557, 1617, 1681 };
static const int rev_ap_base[FX_REV_APS] = { 556, 441 };
static int rev_cl[FX_REV_COMBS], rev_al[FX_REV_APS];
static float rev_cb[2][FX_REV_COMBS][FX_REV_COMB_MAX];
static float rev_clp[2][FX_REV_COMBS];
static int rev_cp[2][FX_REV_COMBS];
static float rev_ab[2][FX_REV_APS][FX_REV_AP_MAX];
static int rev_ap[2][FX_REV_APS];

/* --- tone / tremolo -------------------------------------------------------- */
static vd_svf tone_f[2];
static vd_svf_c tone_c;
static float trem_phase;

void fx_init(float sample_rate)
{
  int i, c, a, ch;

  fx_sr = sample_rate > 0.0f ? sample_rate : 48000.0f;
  fx_ratio = fx_sr / FX_REF_SR;

  dl_len = (int)(0.375f * fx_sr + 0.5f);
  if (dl_len < 64) dl_len = 64;
  if (dl_len > FX_DELAY_MAX) dl_len = FX_DELAY_MAX;

  for (i = 0; i < FX_REV_COMBS; i++) {
    rev_cl[i] = (int)(rev_comb_base[i] * fx_ratio + 0.5f);
    if (rev_cl[i] < 16) rev_cl[i] = 16;
    if (rev_cl[i] > FX_REV_COMB_MAX) rev_cl[i] = FX_REV_COMB_MAX;
  }
  for (i = 0; i < FX_REV_APS; i++) {
    rev_al[i] = (int)(rev_ap_base[i] * fx_ratio + 0.5f);
    if (rev_al[i] < 16) rev_al[i] = 16;
    if (rev_al[i] > FX_REV_AP_MAX) rev_al[i] = FX_REV_AP_MAX;
  }

  for (ch = 0; ch < 2; ch++) {
    ch_pos[ch] = 0;
    dl_pos[ch] = 0;
    dl_lp[ch] = 0.0f;
    tone_f[ch].ic1 = tone_f[ch].ic2 = 0.0f;
    for (i = 0; i < FX_CHORUS_LEN; i++) ch_buf[ch][i] = 0.0f;
    for (i = 0; i < FX_DELAY_MAX; i++) dl_buf[ch][i] = 0.0f;
    for (c = 0; c < FX_REV_COMBS; c++) {
      rev_cp[ch][c] = 0;
      rev_clp[ch][c] = 0.0f;
      for (i = 0; i < FX_REV_COMB_MAX; i++) rev_cb[ch][c][i] = 0.0f;
    }
    for (a = 0; a < FX_REV_APS; a++) {
      rev_ap[ch][a] = 0;
      for (i = 0; i < FX_REV_AP_MAX; i++) rev_ab[ch][a][i] = 0.0f;
    }
  }
  ch_phase = 0.0f;
  trem_phase = 0.0f;
  tone_c.k = tone_c.a1 = tone_c.a2 = tone_c.a3 = 0.0f;
}

/* Linear read from a circular buffer at `pos - delay`, wrapped once. */
static float chorus_tap(const float *buf, int pos, float delay)
{
  float p = (float)pos - delay;
  int i0, i1;
  float frac;
  if (p < 0.0f) p += (float)FX_CHORUS_LEN;
  i0 = (int)p;
  i1 = i0 + 1;
  if (i1 >= FX_CHORUS_LEN) i1 -= FX_CHORUS_LEN;
  frac = p - (float)i0;
  return buf[i0] + (buf[i1] - buf[i0]) * frac;
}

static void chorus_run(float *l, float *r, int n, float mix)
{
  float base1 = 700.0f * fx_ratio, depth1 = 125.0f * fx_ratio;
  float base2 = 520.0f * fx_ratio, depth2 = 100.0f * fx_ratio;
  float step = 0.37f / fx_sr;
  float dry = 1.0f - 0.40f * mix, wet = 0.85f * mix;
  int i;

  for (i = 0; i < n; i++) {
    float xl = l[i], xr = r[i];
    float p1 = vd_sin(ch_phase);
    float p2 = vd_sin(ch_phase + 0.5f);
    float t1 = chorus_tap(ch_buf[0], ch_pos[0], base1 + depth1 * p1);
    float t2 = chorus_tap(ch_buf[1], ch_pos[1], base2 + depth2 * p2);

    ch_buf[0][ch_pos[0]] = xl;
    ch_buf[1][ch_pos[1]] = xr;
    if (++ch_pos[0] >= FX_CHORUS_LEN) ch_pos[0] = 0;
    if (++ch_pos[1] >= FX_CHORUS_LEN) ch_pos[1] = 0;

    l[i] = xl * dry + t1 * wet;
    r[i] = xr * dry + t2 * wet;
    ch_phase += step;
    if (ch_phase >= 1.0f) ch_phase -= 1.0f;
  }
}

static void delay_run(float *l, float *r, int n, float mix, float fb)
{
  int i;

  for (i = 0; i < n; i++) {
    float xl = l[i], xr = r[i];
    float yl = dl_buf[0][dl_pos[0]];
    float yr = dl_buf[1][dl_pos[1]];

    dl_lp[0] += (yl - dl_lp[0]) * 0.55f;
    dl_lp[1] += (yr - dl_lp[1]) * 0.55f;
    dl_buf[0][dl_pos[0]] = xl + dl_lp[1] * fb;
    dl_buf[1][dl_pos[1]] = xr + dl_lp[0] * fb;
    if (++dl_pos[0] >= dl_len) dl_pos[0] = 0;
    if (++dl_pos[1] >= dl_len) dl_pos[1] = 0;

    l[i] = xl + yl * mix;
    r[i] = xr + yr * mix;
  }
}

static void reverb_run(float *l, float *r, int n, float mix, float fb, float damp)
{
  int i, c, a;

  for (i = 0; i < n; i++) {
    float xl = l[i], xr = r[i];
    float sl = 0.0f, sr = 0.0f;
    float wl, wr;

    for (c = 0; c < FX_REV_COMBS; c++) {
      int len = rev_cl[c];
      int p = rev_cp[0][c];
      float y = rev_cb[0][c][p];
      float lp = rev_clp[0][c] + (y - rev_clp[0][c]) * damp;
      rev_clp[0][c] = lp;
      rev_cb[0][c][p] = (xl + xr * 0.18f) + lp * fb;
      if (++p >= len) p = 0;
      rev_cp[0][c] = p;
      sl += y;

      p = rev_cp[1][c];
      y = rev_cb[1][c][p];
      lp = rev_clp[1][c] + (y - rev_clp[1][c]) * damp;
      rev_clp[1][c] = lp;
      rev_cb[1][c][p] = (xr + xl * 0.18f) + lp * fb;
      if (++p >= len) p = 0;
      rev_cp[1][c] = p;
      sr += y;
    }

    wl = sl * (1.0f / (float)FX_REV_COMBS);
    wr = sr * (1.0f / (float)FX_REV_COMBS);
    for (a = 0; a < FX_REV_APS; a++) {
      int len = rev_al[a];
      const float k = 0.5f;
      int p = rev_ap[0][a];
      float in = wl;
      float y = rev_ab[0][a][p];
      wl = y - k * in;
      rev_ab[0][a][p] = in + k * wl; /* feed back the OUTPUT: |k| < 1 keeps the pole inside */
      if (++p >= len) p = 0;
      rev_ap[0][a] = p;

      p = rev_ap[1][a];
      in = wr;
      y = rev_ab[1][a][p];
      wr = y - k * in;
      rev_ab[1][a][p] = in + k * wr;
      if (++p >= len) p = 0;
      rev_ap[1][a] = p;
    }

    l[i] = xl * (1.0f - 0.40f * mix) + wl * (1.6f * mix);
    r[i] = xr * (1.0f - 0.40f * mix) + wr * (1.6f * mix);
  }
}

static void tremolo_run(float *l, float *r, int n, float depth)
{
  float step = 5.4f / fx_sr;
  int i;

  for (i = 0; i < n; i++) {
    float ml = 1.0f - depth * 0.5f * (1.0f - vd_sin(trem_phase));
    float mr = 1.0f - depth * 0.5f * (1.0f - vd_sin(trem_phase + 0.08f));
    l[i] *= ml;
    r[i] *= mr;
    trem_phase += step;
    if (trem_phase >= 1.0f) trem_phase -= 1.0f;
  }
}

void fx_process(float *l, float *r, int frames, const int *amounts)
{
  float a[FX_COUNT];
  float fc, cutoff;
  int i;

  for (i = 0; i < FX_COUNT; i++) a[i] = (float)amounts[i] * 0.001f;

  if (a[FX_DRIVE] > 0.0f) {
    float g = 1.0f + 5.0f * a[FX_DRIVE];
    for (i = 0; i < frames; i++) { l[i] *= g; r[i] *= g; }
  }

  if (a[FX_CHORUS] > 0.0f) chorus_run(l, r, frames, a[FX_CHORUS]);

  if (a[FX_DELAY] > 0.0f)
    delay_run(l, r, frames, a[FX_DELAY], 0.85f * a[FX_FEEDBACK]);

  if (a[FX_REVERB] > 0.0f) {
    float decay = a[FX_DECAY];
    reverb_run(l, r, frames, a[FX_REVERB],
               0.55f + 0.34f * decay, 0.35f + 0.60f * decay);
  }

  /* Always tick the filter so bypass/re-enable cannot click its state. */
  cutoff = 500.0f * vd_pow(36.0f, a[FX_TONE]);
  fc = cutoff < fx_sr * 0.45f ? cutoff : fx_sr * 0.45f;
  if (a[FX_TONE] >= 0.999f) fc = fx_sr * 0.45f;
  vd_svf_set(&tone_c, fc, 0.707f, fx_sr);
  for (i = 0; i < frames; i++) {
    l[i] = vd_svf_tick(&tone_f[0], &tone_c, l[i]).lp;
    r[i] = vd_svf_tick(&tone_f[1], &tone_c, r[i]).lp;
  }

  if (a[FX_TREMOLO] > 0.0f) tremolo_run(l, r, frames, a[FX_TREMOLO]);
}
