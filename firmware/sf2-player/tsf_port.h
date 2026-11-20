/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SF2_TSF_PORT_H
#define SF2_TSF_PORT_H

/* TinySoundFont's malloc/free/realloc are mapped onto one of two static arenas.
   Select an arena before loading/configuring a font, reset it before reloading. */
void sf_arena_select(int bank);   /* 0/1, or -1 = no arena active */
void sf_arena_reset(int bank);
int  sf_arena_used(int bank);     /* bytes used, for diagnostics */

#endif
