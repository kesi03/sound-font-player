/* SPDX-License-Identifier: Apache-2.0
 *
 * Master effects chain for SF2 PLAYER. All state is owned by the audio flow;
 * the control flow only publishes 0..1000 amounts through atomic parameters.
 */
#ifndef SF2_FX_H
#define SF2_FX_H

#include "vibe.h"

enum {
  FX_DRIVE = 0,
  FX_CHORUS,
  FX_DELAY,
  FX_FEEDBACK,
  FX_REVERB,
  FX_DECAY,
  FX_TREMOLO,
  FX_TONE,
  FX_COUNT
};

/* Called once from vibe_on_init with the host sample rate. */
void fx_init(float sample_rate);

/* In-place stereo processing. amounts[] is indexed by FX_* and holds 0..1000. */
void fx_process(float *l, float *r, int frames, const int *amounts);

#endif
