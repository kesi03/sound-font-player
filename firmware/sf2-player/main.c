/* SPDX-License-Identifier: Apache-2.0
 *
 * SF2 PLAYER - a VIBE FM SoundFont instrument.
 *
 * Control flow (init, events, draw): media browser, chunked loads, preset
 * menu, editor protocol, FOCUS drawing. Audio flow: note queue, atomic state
 * snapshots, TinySoundFont rendering and a fallback demo voice.
 */
#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"
#include "tsf_config.h"
#include "vendor/tsf.h"
#include "tsf_port.h"

VIBE_PARALLEL_AUDIO

#define SF_MAX_FILE       (16u * 1024u * 1024u)
#define SF_MAX_PRESETS    256
#define SF_NAME_LEN       48
#define SF_MAX_VOICES     64
#define SF_NOTE_QUEUE     256
#define SF_UI_ROWS        8
#define SF_ED_OUT_MAX     4096
#define SF_ED_CHUNK       2048
#define SF_READ_CHUNK     32768
#define SF_READS_PER_TICK 8
#define SF_DEMO_VOICES    16

enum { UI_SOUND, UI_FILES, UI_CTRL, UI_HELP };
enum { LOAD_IDLE, LOAD_STAT, LOAD_READ };
enum { UP_IDLE, UP_DATA };
enum { MSG_ON = 1, MSG_OFF = 2, MSG_ALL = 3, MSG_AUDITION = 4 };
enum { P_PRESET, P_VOLUME, P_OCTAVE, P_TRANSPOSE, P_COUNT };

#define C_BG     0x0B0F17
#define C_PANEL  0x151C2A
#define C_PANEL2 0x202A3C
#define C_ACC    0xF0A64A
#define C_FG     0xE9EEF7
#define C_MUTED  0x8B95A7
#define C_DIM    0x5B6577
#define C_GREEN  0x6EE7A8
#define C_RED    0xFF6B6B
#define C_BLUE   0x5AB6FF

static const char *const GROUPS[] = { "SOUND", "SOUND", "SOUND", "SOUND" };
static const vibe_param PARAMS[] = {
  { "preset", "Preset", 0.0f, 255.0f, 0.0f, "", 0, 0 },
  { "volume", "Volume", 0.0f, 2.0f, 1.0f, "", 0, 0 },
  { "octave", "Octave", -2.0f, 2.0f, 0.0f, "", 5, 0 },
  { "transpose", "Transpose", -24.0f, 24.0f, 0.0f, "st", 0, 0 }
};

static const char *const HELP[] = {
  "KEYS / PADS", "play the selected preset", "DATA", "browse presets or files",
  "ENTER", "audition / open a file", "F1 / F2", "previous / next preset",
  "F3", "audition middle C", "F4", "toggle SOUND / FILES",
  "F5 / K5", "this overview", "F6 / K6", "all notes off",
  "K1-K3", "SOUND / FILES / CTRL pages", "KNOBS 1-4", "preset / volume / octave / transpose",
  "HOLD ENTER", "turn on FOCUS, DATA walks controls", "RETURN", "back / FOCUS off",
  "HOME", "this overview", "EDITOR", "upload .sf2 and pick a preset",
  "SPEED", "two static 16 MB font banks"
};

static float sr = 48000.0f;

/* ---- values shared by the two flows ---------------------------------------- */
/* Counters and data are published data-first, then VIBE_RELEASE(), then the
   counter. The audio flow loads the counter, VIBE_ACQUIRE(), then reads data. */
static int g_swap_req, g_swap_ack;
static int g_swap_bank, g_swap_preset;
static int g_active_bank;
static int g_preset_req;
static int g_volume_i = 1000;
static int g_octave_i;
static int g_transpose_i;

typedef struct { uint8_t type, note, vel, pad; } note_msg;
static note_msg note_q[SF_NOTE_QUEUE];
static int q_head, q_tail;

static tsf *fonts[2];
static float render_buf[VIBE_MAX_BLOCK * 2];

/* Control-flow-only state. */
static int ui_page = UI_SOUND, help;
static int sel_preset, top_preset, sel_file, top_file;
static int npresets;
static char status[128];
static char font_name[64];
static char preset_names[SF_MAX_PRESETS][SF_NAME_LEN];

static char sources[1024], src_keys[16][48];
static int nsrc, src_want = 1, list_want;
static char cur_dir[VIBE_MEDIA_PATH];
static char entries[24][128];
static char listbuf[8192];
static int nentries;

static int load_state = LOAD_IDLE, load_bank, load_len;
static uint64_t load_size;
static char load_path[VIBE_MEDIA_PATH];
static char load_name[128];
static uint8_t filebuf[SF_MAX_FILE];

static int up_state = UP_IDLE, up_len, up_off;
static char up_name[128];
static uint8_t ed_out[SF_ED_OUT_MAX];
static int ed_out_len, ed_pending;

/* Audio-flow-only state. */
typedef struct { int note; float env, vel, phase; uint8_t on; } demo_voice;
static demo_voice demo[SF_DEMO_VOICES];
static int cur_bank, cur_preset, cur_vol = -1;
static int audition_left, audition_note = 60;

static void work(void);
static void ed_queue_status(void);
static void set_page(int page);
static void select_preset(int index);
static void apply_param(int id, float value);
static void queue_msg(uint8_t type, int note, int vel);
static void open_sel(void);
static void start_media_load(const char *path);
static void parse_font(void);
static void fail_load(const char *why);
static int  inactive_bank(void);

/* ---- tiny string helpers (no libc) ----------------------------------------- */

static int scpy(char *d, const char *s, int max)
{
  int n = 0;
  if (!d || max <= 0) return 0;
  if (!s) { d[0] = 0; return 0; }
  while (s[n] && n < max - 1) { d[n] = s[n]; n++; }
  d[n] = 0;
  return n;
}

static int scat(char *d, const char *s, int max)
{
  int n = 0;
  if (!d || max <= 0) return 0;
  while (d[n] && n < max - 1) n++;
  return n + scpy(d + n, s, max - n);
}

static const char *field(const char *line, int k)
{
  while (k-- > 0) {
    while (*line && *line != '\t') line++;
    if (*line) line++;
  }
  return line;
}

static int field_copy(const char *line, int k, char *dst, int max)
{
  const char *s = field(line, k);
  int n = 0;
  if (!dst || max <= 0) return 0;
  while (s[n] && s[n] != '\t' && n < max - 1) { dst[n] = s[n]; n++; }
  dst[n] = 0;
  return n;
}

static const char *base_of(const char *p)
{
  const char *b = p, *q;
  for (q = p; *q; q++) if (*q == '/') b = q + 1;
  return b;
}

static int ext_is(const char *p, const char *ext)
{
  const char *dot = 0, *q;
  if (!p) return 0;
  for (q = p; *q; q++) if (*q == '.') dot = q;
  if (!dot) return 0;
  dot++;
  while (*dot && *ext) {
    char a = *dot, b = *ext;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (a != b) return 0;
    dot++; ext++;
  }
  return *dot == 0 && *ext == 0;
}

static int streq_n(const char *a, const char *b, int n)
{
  int i;
  for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
  return 1;
}

static void buf_copy(uint8_t *dst, const uint8_t *src, int n)
{
  int i;
  for (i = 0; i < n; i++) dst[i] = src[i];
}

static uint32_t le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void log_line(const char *a, const char *b)
{
  char m[272];
  scpy(m, a, sizeof m);
  scat(m, b, sizeof m);
  vibe_log(m);
}

/* ---- editor status ---------------------------------------------------------- */

static void ed_queue_status(void)
{
  char text[SF_ED_OUT_MAX - 1];
  char t[24];
  int n = 0, i;

  text[0] = 0;
  scat(text, status, sizeof text);
  scat(text, "\nFONT:", sizeof text);
  scat(text, font_name[0] ? font_name : "(none)", sizeof text);
  scat(text, "\n", sizeof text);
  for (i = 0; i < npresets && n < (int)sizeof text - 60; i++) {
    scat(text, vg_itoa(i, t), sizeof text);
    scat(text, ":", sizeof text);
    scat(text, preset_names[i], sizeof text);
    scat(text, "\n", sizeof text);
    n = 0;
    while (text[n]) n++;
  }
  n = 0;
  while (text[n]) n++;
  if (n > (int)sizeof text - 1) n = (int)sizeof text - 1;
  ed_out[0] = 2;
  for (i = 0; i < n; i++) ed_out[1 + i] = (uint8_t)text[i];
  ed_out_len = 1 + n;
  ed_pending = 1;
}

static void ed_flush(void)
{
  if (!ed_pending) return;
  if (vibe_editor_post(ed_out, ed_out_len) == 0) ed_pending = 0;
}

/* ---- shared-state helpers (control flow) ----------------------------------- */

static void queue_msg(uint8_t type, int note, int vel)
{
  int h = __atomic_load_n(&q_head, __ATOMIC_RELAXED);
  int t = __atomic_load_n(&q_tail, __ATOMIC_ACQUIRE);
  note_msg m;
  if (h - t >= SF_NOTE_QUEUE) return;
  m.type = type;
  m.note = (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
  m.vel = (uint8_t)(vel < 0 ? 0 : vel > 127 ? 127 : vel);
  m.pad = 0;
  note_q[h % SF_NOTE_QUEUE] = m;
  VIBE_RELEASE();
  __atomic_store_n(&q_head, h + 1, __ATOMIC_RELEASE);
}

static int active_bank_snapshot(void)
{
  return __atomic_load_n(&g_active_bank, __ATOMIC_ACQUIRE) & 1;
}

static int swap_pending(void)
{
  return __atomic_load_n(&g_swap_req, __ATOMIC_ACQUIRE) !=
         __atomic_load_n(&g_swap_ack, __ATOMIC_ACQUIRE);
}

static int inactive_bank(void)
{
  if (swap_pending()) return -1;
  return 1 - active_bank_snapshot();
}

static void select_preset(int index)
{
  char t[24];
  if (npresets <= 0) {
    scpy(status, "no soundfont loaded", sizeof status);
    ed_queue_status();
    return;
  }
  if (index < 0) index = 0;
  if (index >= npresets) index = npresets - 1;
  sel_preset = index;
  top_preset = sel_preset;
  if (top_preset > npresets - SF_UI_ROWS) top_preset = npresets - SF_UI_ROWS;
  if (top_preset < 0) top_preset = 0;
  __atomic_store_n(&g_preset_req, index, __ATOMIC_RELEASE);
  vibe_param_set(P_PRESET, (float)index);
  scpy(status, "preset ", sizeof status);
  scat(status, vg_itoa(index, t), sizeof status);
  scat(status, ": ", sizeof status);
  scat(status, preset_names[index], sizeof status);
  ed_queue_status();
}

static void set_volume(float v)
{
  int n = (int)(v * 1000.0f + 0.5f);
  if (n < 0) n = 0;
  if (n > 2000) n = 2000;
  __atomic_store_n(&g_volume_i, n, __ATOMIC_RELEASE);
}

static void apply_param(int id, float value)
{
  int n;
  switch (id) {
    case P_PRESET:
      select_preset((int)(value + 0.5f));
      break;
    case P_VOLUME:
      set_volume(value);
      break;
    case P_OCTAVE:
      n = (int)(value + 0.5f);
      if (n < -2) n = -2;
      if (n > 2) n = 2;
      g_octave_i = n;
      vibe_keys_octave(2 + n);
      break;
    case P_TRANSPOSE:
      n = (int)(value + 0.5f);
      if (n < -24) n = -24;
      if (n > 24) n = 24;
      g_transpose_i = n;
      break;
    default:
      break;
  }
}

static void change_param(int id, float delta)
{
  float v;
  if (id < 0 || id >= P_COUNT) return;
  v = vibe_param_get(id) + delta;
  if (v < PARAMS[id].min) v = PARAMS[id].min;
  if (v > PARAMS[id].max) v = PARAMS[id].max;
  vibe_param_set(id, v);
  apply_param(id, v);
}

static void set_page(int page)
{
  static const char *const K[] = { "SOUND", "FILES", "CTRL", "AUDITION", "HELP", "ALL OFF" };
  static const char *const F_SOUND[] = { "PREV", "NEXT", "AUDIT", "FILES", "HELP", "ALL OFF" };
  static const char *const F_FILES[] = { "BACK", "OPEN", "AUDIT", "SOUND", "HELP", "ALL OFF" };
  static const char *const F_CTRL[]  = { "PREV", "NEXT", "AUDIT", "FILES", "HELP", "ALL OFF" };
  static const char *const F_HELP[]  = { "SOUND", "FILES", "CTRL", "AUDIT", "CLOSE", "ALL OFF" };
  const char *const *f = F_SOUND;
  int i;

  ui_page = page;
  help = page == UI_HELP;
  if (page == UI_FILES) f = F_FILES;
  else if (page == UI_CTRL) f = F_CTRL;
  else if (page == UI_HELP) f = F_HELP;
  for (i = 0; i < 6; i++) {
    vibe_label(VB_F1 + i, f[i]);
    vibe_label(VB_K1 + i, K[i]);
  }
  if (page == UI_FILES && !cur_dir[0]) src_want = 1;
}

static void audition(void)
{
  queue_msg(MSG_AUDITION, 60, 100);
  scpy(status, "audition: middle C", sizeof status);
}

static void all_off(void)
{
  queue_msg(MSG_ALL, 0, 0);
  scpy(status, "all notes off", sizeof status);
}

/* ---- media browser --------------------------------------------------------- */

static void parse_sources(const char *buf, int len)
{
  const char *p = buf, *end = buf + len;
  nsrc = 0;
  if (!cur_dir[0]) nentries = 0;
  while (p < end && nsrc < (int)(sizeof src_keys / sizeof src_keys[0])) {
    const char *nl = p;
    while (nl < end && *nl != '\n') nl++;
    if (nl > p && field_copy(p, 0, src_keys[nsrc], sizeof src_keys[0])) {
      if (!cur_dir[0] && nentries < (int)(sizeof entries / sizeof entries[0])) {
        scpy(entries[nentries], src_keys[nsrc], sizeof entries[nentries]);
        nentries++;
      }
      nsrc++;
    }
    p = nl < end ? nl + 1 : nl;
  }
  if (!cur_dir[0]) sel_file = top_file = 0;
}

static int media_absolute(const char *p)
{
  int n = 0, k, m;
  while (p[n] && p[n] != '/') n++;
  if (!p[n] || n < 1 || n >= (int)sizeof src_keys[0]) return 0;
  for (k = 0; k < nsrc; k++) {
    m = 0;
    while (src_keys[k][m]) m++;
    if (m == n && streq_n(p, src_keys[k], n)) return 1;
  }
  return 0;
}

static void parse_entries(const char *buf, int len)
{
  const char *p = buf, *end = buf + len;
  nentries = 0;
  while (p < end && nentries < (int)(sizeof entries / sizeof entries[0])) {
    const char *nl = p;
    int n;
    while (nl < end && *nl != '\n') nl++;
    n = (int)(nl - p);
    if (n > 127) n = 127;
    for (int i = 0; i < n; i++) entries[nentries][i] = p[i];
    entries[nentries][n] = 0;
    nentries++;
    p = nl < end ? nl + 1 : nl;
  }
  if (sel_file >= nentries) sel_file = nentries > 0 ? nentries - 1 : 0;
  if (sel_file < 0) sel_file = 0;
  if (sel_file < top_file) top_file = sel_file;
  if (sel_file >= top_file + SF_UI_ROWS) top_file = sel_file - SF_UI_ROWS + 1;
}

static void open_browser(void)
{
  set_page(UI_FILES);
  cur_dir[0] = 0;
  nentries = 0;
  sel_file = top_file = 0;
  src_want = 1;
  list_want = 0;
  scpy(status, "choose a .sf2 file", sizeof status);
  ed_queue_status();
}

static void browser_up(void)
{
  int n;
  if (ui_page != UI_FILES) return;
  if (!cur_dir[0]) { set_page(UI_SOUND); return; }
  n = 0;
  while (cur_dir[n]) n++;
  while (n > 0 && cur_dir[n - 1] != '/') n--;
  if (n > 0) cur_dir[n - 1] = 0;
  else cur_dir[0] = 0;
  nentries = 0;
  sel_file = top_file = 0;
  src_want = cur_dir[0] ? 0 : 1;
  list_want = cur_dir[0] ? 1 : 0;
  scpy(status, cur_dir[0] ? cur_dir : "media sources", sizeof status);
}

static void start_media_load(const char *path)
{
  int bank;
  if (!path || !path[0]) return;
  if (!ext_is(base_of(path), "sf2")) {
    scpy(status, "only .sf2 soundfonts are supported", sizeof status);
    ed_queue_status();
    return;
  }
  if (load_state != LOAD_IDLE || up_state != UP_IDLE) {
    scpy(status, "loader busy", sizeof status);
    ed_queue_status();
    return;
  }
  bank = inactive_bank();
  if (bank < 0) {
    scpy(status, "wait for the font switch", sizeof status);
    ed_queue_status();
    return;
  }
  scpy(load_path, path, sizeof load_path);
  scpy(load_name, base_of(path), sizeof load_name);
  load_bank = bank;
  load_size = 0;
  load_len = 0;
  load_state = LOAD_STAT;
  scpy(status, "loading ", sizeof status);
  scat(status, load_name, sizeof status);
  log_line("sf2 load: ", load_name);
  ed_queue_status();
  work();
}

static void open_sel(void)
{
  char path[VIBE_MEDIA_PATH];
  char name[128];
  if (sel_file < 0 || sel_file >= nentries) return;
  if (!cur_dir[0]) {
    field_copy(entries[sel_file], 0, cur_dir, sizeof cur_dir);
    nentries = 0;
    sel_file = top_file = 0;
    list_want = 1;
    scpy(status, cur_dir, sizeof status);
    return;
  }
  field_copy(entries[sel_file], 2, name, sizeof name);
  scpy(path, cur_dir, sizeof path);
  if (path[0]) scat(path, "/", sizeof path);
  scat(path, name, sizeof path);
  if (entries[sel_file][0] == 'd') {
    scpy(cur_dir, path, sizeof cur_dir);
    nentries = 0;
    sel_file = top_file = 0;
    list_want = 1;
    scpy(status, cur_dir, sizeof status);
    return;
  }
  start_media_load(path);
}

/* ---- font loading ---------------------------------------------------------- */

static void fail_load(const char *why)
{
  if (load_state != LOAD_IDLE) fonts[load_bank] = 0;
  load_state = LOAD_IDLE;
  scpy(status, why, sizeof status);
  log_line("sf2 load failed: ", why);
  ed_queue_status();
}

static void parse_font(void)
{
  tsf *f;
  int ok, n, i;

  load_state = LOAD_IDLE;
  if (load_len < 12 || load_len > (int)SF_MAX_FILE) {
    fail_load("bad soundfont size");
    return;
  }
  sf_arena_reset(load_bank);
  fonts[load_bank] = 0;
  sf_arena_select(load_bank);
  f = tsf_load_memory(filebuf, load_len);
  if (f) {
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, (int)sr, 0.0f);
    ok = tsf_set_max_voices(f, SF_MAX_VOICES);
  } else ok = 0;
  sf_arena_select(-1);
  if (!f || !ok) {
    fail_load("invalid .sf2 or not enough font memory");
    return;
  }
  fonts[load_bank] = f;

  n = tsf_get_presetcount(f);
  if (n > SF_MAX_PRESETS) n = SF_MAX_PRESETS;
  npresets = n;
  for (i = 0; i < n; i++) {
    const char *nm = tsf_get_presetname(f, i);
    scpy(preset_names[i], nm && *nm ? nm : "(unnamed)", SF_NAME_LEN);
  }
  scpy(font_name, load_name, sizeof font_name);
  if (sel_preset >= npresets) sel_preset = 0;
  top_preset = sel_preset;

  g_swap_bank = load_bank;
  g_swap_preset = sel_preset;
  VIBE_RELEASE();
  __atomic_fetch_add(&g_swap_req, 1, __ATOMIC_RELEASE);

  scpy(status, "loaded ", sizeof status);
  scat(status, font_name, sizeof status);
  {
    char t[24];
    scat(status, " - ", sizeof status);
    scat(status, vg_itoa(n, t), sizeof status);
    scat(status, " presets", sizeof status);
  }
  log_line("sf2 loaded: ", font_name);
  ed_queue_status();
}

static void editor_commit(void)
{
  int bank;
  if (up_state != UP_DATA || up_off != up_len) {
    up_state = UP_IDLE;
    fail_load("upload incomplete");
    return;
  }
  bank = inactive_bank();
  if (bank < 0 || load_state != LOAD_IDLE) {
    up_state = UP_IDLE;
    scpy(status, "loader busy - upload discarded", sizeof status);
    ed_queue_status();
    return;
  }
  scpy(load_name, up_name, sizeof load_name);
  load_path[0] = 0;
  load_len = up_len;
  load_bank = bank;
  up_state = UP_IDLE;
  parse_font();
}

/* ---- editor protocol ------------------------------------------------------- */

static void handle_editor_data(const uint8_t *p, int n)
{
  if (!p || n < 1) return;
  switch (p[0]) {
    case 0x10: { /* header: cmd, len:u32, name_len, name... */
      int name_len;
      if (n < 6) return;
      up_len = (int)le32(p + 1);
      name_len = p[5];
      if (n < 6 + name_len || up_len < 12 || up_len > (int)SF_MAX_FILE || name_len > 127) {
        up_state = UP_IDLE;
        scpy(status, "bad upload header", sizeof status);
        ed_queue_status();
        return;
      }
      if (inactive_bank() < 0 || load_state != LOAD_IDLE) {
        scpy(status, "loader busy - upload rejected", sizeof status);
        ed_queue_status();
        return;
      }
      for (int i = 0; i < name_len; i++) up_name[i] = (char)p[6 + i];
      up_name[name_len] = 0;
      up_off = 0;
      up_state = UP_DATA;
      scpy(status, "uploading ", sizeof status);
      scat(status, up_name, sizeof status);
      ed_queue_status();
      break;
    }
    case 0x11: { /* chunk: cmd, offset:u32, data... */
      int off, len;
      if (up_state != UP_DATA || n < 5) return;
      off = (int)le32(p + 1);
      len = n - 5;
      if (len < 1 || off != up_off || off + len > up_len) {
        up_state = UP_IDLE;
        scpy(status, "bad upload chunk", sizeof status);
        ed_queue_status();
        return;
      }
      buf_copy(filebuf + off, p + 5, len);
      up_off += len;
      break;
    }
    case 0x12: /* commit */
      editor_commit();
      break;
    case 0x13: /* cancel */
      up_state = UP_IDLE;
      scpy(status, "upload cancelled", sizeof status);
      ed_queue_status();
      break;
    case 0x20: /* status request */
      ed_queue_status();
      break;
    case 0x21: /* select preset: cmd, index:u32 */
      if (n >= 5) select_preset((int)le32(p + 1));
      break;
    case 0x22: /* all notes off */
      all_off();
      ed_queue_status();
      break;
    case 0x23: /* audition: cmd, note */
      queue_msg(MSG_AUDITION, n >= 2 ? p[1] : 60, 100);
      break;
    case 0x30: { /* load a media path: cmd, UTF-8 path */
      char path[VIBE_MEDIA_PATH];
      int i, len = n - 1;
      if (len < 1 || len >= (int)sizeof path) return;
      for (i = 0; i < len; i++) path[i] = (char)p[1 + i];
      path[len] = 0;
      if (!media_absolute(path) && nsrc > 0) {
        char full[VIBE_MEDIA_PATH];
        scpy(full, src_keys[0], sizeof full);
        scat(full, "/", sizeof full);
        scat(full, path, sizeof full);
        scpy(path, full, sizeof path);
      }
      start_media_load(path);
      break;
    }
    default:
      break;
  }
}

/* ---- work on the control flow --------------------------------------------- */

static void work(void)
{
  int r = 0, k;

  if (src_want) {
    r = vibe_media_sources(sources, (int)sizeof sources - 1);
    if (r == VIBE_MEDIA_PENDING) { ed_flush(); return; }
    src_want = 0;
    if (r < 0) {
      nsrc = 0;
      nentries = 0;
      scpy(status, "this host has no media (use the Editor to upload)", sizeof status);
      ed_queue_status();
      ed_flush();
      return;
    }
    sources[r] = 0;
    parse_sources(sources, r);
    if (cur_dir[0] && !media_absolute(cur_dir)) {
      cur_dir[0] = 0;
      nentries = 0;
      src_want = 1;
    }
  }

  if (list_want && cur_dir[0]) {
    r = vibe_media_list(cur_dir, 0, listbuf, (int)sizeof listbuf - 1);
    if (r == VIBE_MEDIA_PENDING) { ed_flush(); return; }
    list_want = 0;
    if (r < 0) {
      nentries = 0;
      scpy(status, "the folder is gone", sizeof status);
      browser_up();
    } else {
      listbuf[r] = 0;
      parse_entries(listbuf, r);
    }
  }

  if (load_state == LOAD_STAT) {
    int64_t mtime = 0;
    r = vibe_media_stat(load_path, &load_size, &mtime);
    if (r == VIBE_MEDIA_PENDING) { ed_flush(); return; }
    if (r < 0) { fail_load("file not found"); ed_flush(); return; }
    if (load_size < 12 || load_size > SF_MAX_FILE) {
      char t[24];
      scpy(status, "file too large: ", sizeof status);
      scat(status, vg_itoa((int)(load_size >> 20), t), sizeof status);
      scat(status, " MB (16 MB max)", sizeof status);
      fail_load(status);
      ed_flush();
      return;
    }
    load_len = 0;
    load_state = LOAD_READ;
  }

  if (load_state == LOAD_READ) {
    static uint8_t chunk[SF_READ_CHUNK];
    int reads = 0;
    while (reads < SF_READS_PER_TICK && (uint64_t)load_len < load_size) {
      int want = SF_READ_CHUNK;
      if ((uint64_t)(load_len + want) > load_size)
        want = (int)(load_size - (uint64_t)load_len);
      r = vibe_media_read(load_path, (uint64_t)load_len, chunk, want);
      if (r == VIBE_MEDIA_PENDING) break;
      if (r < 0) { fail_load("file disappeared while reading"); break; }
      if (r == 0) break;
      if (load_len + r > (int)SF_MAX_FILE) { fail_load("file too large"); break; }
      buf_copy(filebuf + load_len, chunk, r);
      load_len += r;
      reads++;
    }
    if (load_state == LOAD_READ && (uint64_t)load_len >= load_size && load_len >= 12) {
      parse_font();
    } else if (load_state == LOAD_READ && r == 0 && load_len >= 12) {
      load_size = (uint64_t)load_len;
      parse_font();
    }
  }

  /* Clamp menus after a font change. */
  if (npresets > 0) {
    if (sel_preset >= npresets) sel_preset = npresets - 1;
    if (top_preset > npresets - SF_UI_ROWS) top_preset = npresets > SF_UI_ROWS ? npresets - SF_UI_ROWS : 0;
    if (top_preset < 0) top_preset = 0;
  } else { sel_preset = top_preset = 0; }
  if (nentries > 0) {
    if (sel_file >= nentries) sel_file = nentries - 1;
    if (top_file > nentries - SF_UI_ROWS) top_file = nentries > SF_UI_ROWS ? nentries - SF_UI_ROWS : 0;
  } else { sel_file = top_file = 0; }

  for (k = 0; k < 4; k++) { /* a few passes so READY can carry straight on */
    int before = (load_state << 16) | (src_want << 8) | list_want;
    if (load_state == LOAD_READ) {
      static uint8_t chunk[SF_READ_CHUNK];
      int got = 0;
      while (got < 2 && load_state == LOAD_READ && (uint64_t)load_len < load_size) {
        int want = SF_READ_CHUNK;
        if ((uint64_t)(load_len + want) > load_size) want = (int)(load_size - (uint64_t)load_len);
        r = vibe_media_read(load_path, (uint64_t)load_len, chunk, want);
        if (r == VIBE_MEDIA_PENDING) break;
        if (r < 0) { fail_load("file disappeared while reading"); break; }
        if (r == 0) break;
        buf_copy(filebuf + load_len, chunk, r);
        load_len += r;
        got++;
      }
      if (load_state == LOAD_READ && (uint64_t)load_len >= load_size && load_len >= 12) parse_font();
      else if (load_state == LOAD_READ && r == 0 && load_len >= 12) { load_size = (uint64_t)load_len; parse_font(); }
    }
    if (before == ((load_state << 16) | (src_want << 8) | list_want)) break;
  }
  ed_flush();
}

/* ---- events --------------------------------------------------------------- */

static void move_sel(int d)
{
  if (ui_page == UI_FILES) {
    sel_file += d;
    if (sel_file < 0) sel_file = 0;
    if (sel_file >= nentries) sel_file = nentries > 0 ? nentries - 1 : 0;
    if (sel_file < top_file) top_file = sel_file;
    if (sel_file >= top_file + SF_UI_ROWS) top_file = sel_file - SF_UI_ROWS + 1;
  } else {
    if (npresets <= 0) return;
    sel_preset += d;
    if (sel_preset < 0) sel_preset = 0;
    if (sel_preset >= npresets) sel_preset = npresets - 1;
    if (sel_preset < top_preset) top_preset = sel_preset;
    if (sel_preset >= top_preset + SF_UI_ROWS) top_preset = sel_preset - SF_UI_ROWS + 1;
    __atomic_store_n(&g_preset_req, sel_preset, __ATOMIC_RELEASE);
    vibe_param_set(P_PRESET, (float)sel_preset);
  }
}

static void handle_button(const vibe_event *e)
{
  int id = e->id;
  if (!e->down) return;

  if (id >= VB_K1 && id <= VB_K6) {
    switch (id - VB_K1) {
      case 0: set_page(UI_SOUND); break;
      case 1: open_browser(); break;
      case 2: set_page(UI_CTRL); break;
      case 3: audition(); break;
      case 4: set_page(UI_HELP); break;
      case 5: all_off(); break;
    }
    return;
  }
  if (id >= VB_F1 && id <= VB_F6) {
    switch (id - VB_F1) {
      case 0:
        if (help) set_page(UI_SOUND);
        else if (ui_page == UI_FILES) browser_up();
        else move_sel(-1);
        break;
      case 1:
        if (ui_page == UI_FILES) open_sel();
        else move_sel(1);
        break;
      case 2: audition(); break;
      case 3: set_page(ui_page == UI_FILES ? UI_SOUND : UI_FILES); break;
      case 4: set_page(UI_HELP); break;
      case 5: all_off(); break;
    }
    return;
  }
  switch (id) {
    case VB_ENTER:
    case VB_DATA_PUSH:
      if (ui_page == UI_FILES) open_sel();
      else if (npresets > 0) {
        __atomic_store_n(&g_preset_req, sel_preset, __ATOMIC_RELEASE);
        audition();
      }
      break;
    case VB_RETURN:
      if (help) set_page(UI_SOUND);
      else if (ui_page == UI_FILES) browser_up();
      else set_page(UI_SOUND);
      break;
    case VB_HOME:
      set_page(ui_page == UI_HELP ? UI_SOUND : UI_HELP);
      break;
    case VB_MENU:
      set_page(ui_page == UI_SOUND ? UI_FILES : ui_page == UI_FILES ? UI_CTRL : UI_SOUND);
      break;
    case VB_LEFT: move_sel(-1); break;
    case VB_RIGHT: move_sel(1); break;
    case VB_OCT_DOWN: change_param(P_OCTAVE, -1.0f); break;
    case VB_OCT_UP: change_param(P_OCTAVE, 1.0f); break;
    default: break;
  }
}

static void handle_focus_press(int id)
{
  char t[24];
  if (id >= 100 && id < 100 + SF_UI_ROWS && ui_page == UI_SOUND) {
    select_preset(top_preset + id - 100);
  } else if (id >= 200 && id < 200 + SF_UI_ROWS && ui_page == UI_FILES) {
    sel_file = top_file + id - 200;
    (void)t;
    open_sel();
  } else if (id == 90) move_sel(-1);
  else if (id == 91) move_sel(1);
  else if (id >= 300 && id < 304) {
    int p = P_PRESET + (id - 300);
    float v = vibe_param_get(p) + (p == P_VOLUME ? 0.1f : 1.0f);
    if (v > PARAMS[p].max) v = PARAMS[p].min;
    vibe_param_set(p, v);
    apply_param(p, v);
  }
}

void vibe_on_event(const vibe_event *e)
{
  int fid = -1;
  int fr = vf_event(e, &fid);
  if (fr == VF_PRESS) { handle_focus_press(fid); return; }
  if (fr != VF_NO) return;

  if (e->type == VIBE_EV_NOTE) {
    int note = e->id + g_transpose_i;
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    if (e->down) queue_msg(MSG_ON, note, (int)(e->value * 127.0f + 0.5f));
    else queue_msg(MSG_OFF, note, 0);
    return;
  }
  if (e->type == VIBE_EV_PARAM) { apply_param(e->id, e->value); return; }
  if (e->type == VIBE_EV_EDITOR_DATA) {
    handle_editor_data(vibe_editor_data(), e->value);
    ed_flush();
    return;
  }
  if (e->type == VIBE_EV_MEDIA) {
    if (e->id == VIBE_MEDIA_EV_SOURCES) src_want = 1;
    if (e->id == VIBE_MEDIA_EV_READY) work();
    if (e->id == VIBE_MEDIA_EV_DROPPED) {
      char path[VIBE_MEDIA_PATH];
      int pad, x, y;
      while (vibe_media_dropped(path, sizeof path, &pad, &x, &y) > 0) {
        start_media_load(path);
      }
    }
    work();
    return;
  }
  if (e->type == VIBE_EV_BUTTON) { handle_button(e); return; }
  if (e->type == VIBE_EV_ENCODER) {
    switch (e->id) {
      case VE_1: move_sel(e->value > 0 ? 1 : -1); break;
      case VE_2: change_param(P_VOLUME, e->value * 0.05f); break;
      case VE_3: change_param(P_OCTAVE, e->value > 0 ? 1.0f : -1.0f); break;
      case VE_4: change_param(P_TRANSPOSE, e->value > 0 ? 1.0f : -1.0f); break;
      default: break;
    }
    return;
  }
}

/* ---- audio ---------------------------------------------------------------- */

static void demo_on(int note, int vel)
{
  int i, free = -1, old = 0;
  float amp = (float)vel / 127.0f * 0.28f;
  for (i = 0; i < SF_DEMO_VOICES; i++) {
    if (!demo[i].on && free < 0) free = i;
    if (demo[i].env > demo[old].env) old = i;
  }
  if (free < 0) free = old;
  demo[free].note = note;
  demo[free].vel = amp;
  demo[free].env = 0.0f;
  demo[free].phase = 0.0f;
  demo[free].on = 1;
}

static void demo_off(int note)
{
  int i;
  for (i = 0; i < SF_DEMO_VOICES; i++) if (demo[i].on && demo[i].note == note) demo[i].on = 0;
}

static void demo_all_off(void)
{
  int i;
  for (i = 0; i < SF_DEMO_VOICES; i++) demo[i].on = 0;
}

static void apply_bank_swap(void)
{
  int req = __atomic_load_n(&g_swap_req, __ATOMIC_ACQUIRE);
  if (req == __atomic_load_n(&g_swap_ack, __ATOMIC_ACQUIRE)) return;
  VIBE_ACQUIRE();
  {
    tsf *f = fonts[g_swap_bank & 1];
    if (f) tsf_note_off_all(f);
    demo_all_off();
    cur_bank = g_swap_bank & 1;
    cur_preset = g_swap_preset;
  }
  VIBE_RELEASE();
  audition_left = 0;
  __atomic_store_n(&g_active_bank, cur_bank, __ATOMIC_RELEASE);
  __atomic_store_n(&g_swap_ack, req, __ATOMIC_RELEASE);
}

static void process_messages(void)
{
  int t = __atomic_load_n(&q_tail, __ATOMIC_RELAXED);
  int h = __atomic_load_n(&q_head, __ATOMIC_ACQUIRE);
  tsf *f = fonts[cur_bank];
  while (t != h) {
    note_msg m;
    VIBE_ACQUIRE();
    m = note_q[t % SF_NOTE_QUEUE];
    VIBE_RELEASE();
    switch (m.type) {
      case MSG_ON:
        if (f) tsf_note_on(f, cur_preset, m.note, (float)m.vel / 127.0f);
        else demo_on(m.note, m.vel);
        break;
      case MSG_OFF:
        if (f) tsf_note_off(f, cur_preset, m.note);
        else demo_off(m.note);
        break;
      case MSG_ALL:
        if (f) tsf_note_off_all(f);
        demo_all_off();
        audition_left = 0;
        break;
      case MSG_AUDITION:
        audition_note = m.note;
        if (f) tsf_note_on(f, cur_preset, m.note, (float)m.vel / 127.0f);
        else demo_on(m.note, m.vel);
        audition_left = (int)(sr * 0.55f);
        break;
      default: break;
    }
    t++;
    __atomic_store_n(&q_tail, t, __ATOMIC_RELEASE);
  }
}

static float output_sample(float x)
{
  float y = vd_tanh(x);
  if (y > 0.999f) return 0.999f;
  if (y < -0.999f) return -0.999f;
  return y;
}

void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames)
{
  tsf *f;
  int preset, vol, i, n;
  float gain;

  (void)in;
  if (frames < 1) return;
  if (frames > VIBE_MAX_BLOCK) frames = VIBE_MAX_BLOCK;

  apply_bank_swap();

  preset = __atomic_load_n(&g_preset_req, __ATOMIC_ACQUIRE);
  if (preset != cur_preset) {
    f = fonts[cur_bank];
    if (f) tsf_note_off_all(f);
    demo_all_off();
    audition_left = 0;
    cur_preset = preset;
  }
  vol = __atomic_load_n(&g_volume_i, __ATOMIC_ACQUIRE);
  if (vol != cur_vol) cur_vol = vol;
  gain = (float)vol * 0.001f;

  process_messages();

  if (audition_left > 0) {
    n = frames < audition_left ? frames : audition_left;
    audition_left -= n;
    if (audition_left <= 0) {
      f = fonts[cur_bank];
      if (f) tsf_note_off(f, cur_preset, audition_note);
      else demo_off(audition_note);
    }
  }

  f = fonts[cur_bank];
  if (f) {
    tsf_render_float(f, render_buf, frames, 0);
    for (i = 0; i < frames; i++) {
      out_l[i] = output_sample(render_buf[i * 2] * gain);
      out_r[i] = output_sample(render_buf[i * 2 + 1] * gain);
    }
  } else {
    for (i = 0; i < frames; i++) {
      float s = 0.0f;
      int v;
      for (v = 0; v < SF_DEMO_VOICES; v++) {
        demo_voice *d = &demo[v];
        float target = d->on ? d->vel : 0.0f;
        float hz;
        if (d->note < 0) continue;
        d->env += (target - d->env) * (d->on ? 0.002f : 0.0004f);
        if (!d->on && d->env < 0.001f) { d->note = -1; continue; }
        hz = 440.0f * vd_exp2((float)(d->note - 69) / 12.0f);
        d->phase += hz / sr;
        if (d->phase >= 1.0f) d->phase -= 1.0f;
        s += (vd_sin(d->phase) * 0.8f + vd_sin(d->phase * 2.0f) * 0.2f) * d->env;
      }
      out_l[i] = out_r[i] = output_sample(s * gain);
    }
  }
}

/* ---- display -------------------------------------------------------------- */

static void draw_softkeys(const char *const labels[6])
{
  static const char *const DEF[6] = { "PREV", "NEXT", "AUDIT", "FILES", "HELP", "ALL OFF" };
  int i;
  if (!labels) labels = DEF;
  for (i = 0; i < 6; i++) {
    int x = 6 + i * 78;
    vg_round(x, 292, 74, 22, 4, C_PANEL2);
    vg_text(labels[i], x + 37, 296, VG_BOLD, C_ACC, VG_CENTER);
    vf_key(VB_F1 + i, x, 290, 74, 26);
  }
}

static void draw_tabs(void)
{
  static const char *const T[] = { "SOUND", "FILES", "CTRL", "HELP" };
  int i;
  for (i = 0; i < 4; i++) {
    int x = 8 + i * 90;
    int on = ui_page == i;
    vg_round(x, 8, 84, 22, 4, on ? C_ACC : C_PANEL2);
    vg_text(T[i], x + 42, 12, VG_BOLD, on ? C_BG : C_MUTED, VG_CENTER);
  }
}

static void draw_knob(int row, int id, const char *label, const char *value)
{
  int y = 86 + row * 42;
  vg_round(300, y, 172, 34, 5, C_PANEL2);
  vg_text(label, 310, y + 3, VG_SMALL, C_MUTED, VG_LEFT);
  vg_text(value, 310, y + 17, VG_BIG, C_FG, VG_LEFT);
  vf_encoder(VE_1 + row, 300, y, 172, 34);
  (void)id;
}

static void draw_sound_page(void)
{
  char t[32];
  int i;
  vg_text(font_name[0] ? font_name : "DEMO VOICE", 366, 12, VG_BOLD, C_ACC, VG_RIGHT);
  vg_text("PRESETS", 12, 42, VG_BOLD, C_MUTED, VG_LEFT);
  vg_text("PREV", 200, 42, VG_SMALL, C_ACC, VG_CENTER);
  vg_text("NEXT", 250, 42, VG_SMALL, C_ACC, VG_CENTER);
  vg_round(188, 40, 42, 22, 4, C_PANEL2);
  vg_round(236, 40, 42, 22, 4, C_PANEL2);
  vg_text("<", 209, 43, VG_BOLD, C_ACC, VG_CENTER);
  vg_text(">", 257, 43, VG_BOLD, C_ACC, VG_CENTER);
  vf_button(90, 188, 39, 42, 24);
  vf_button(91, 236, 39, 42, 24);

  for (i = 0; i < SF_UI_ROWS; i++) {
    int idx = top_preset + i;
    int y = 68 + i * 26;
    if (idx >= npresets) break;
    if (idx == sel_preset) vg_round(8, y, 276, 24, 4, C_ACC);
    vg_text(vg_itoa(idx, t), 16, y + 4, VG_SMALL, idx == sel_preset ? C_BG : C_DIM, VG_LEFT);
    vg_text(preset_names[idx], 52, y + 3, VG_MED, idx == sel_preset ? C_BG : C_FG, VG_LEFT);
    vf_button(100 + i, 8, y, 276, 24);
  }
  if (npresets == 0) vg_text("no .sf2 loaded - keys play a demo voice", 16, 90, VG_SMALL, C_GREEN, VG_LEFT);

  vg_round(292, 40, 180, 38, 5, C_PANEL);
  vg_text("SELECTED", 302, 44, VG_SMALL, C_MUTED, VG_LEFT);
  vg_text(npresets ? preset_names[sel_preset] : "DEMO", 302, 58, VG_BIG, C_ACC, VG_LEFT);

  draw_knob(0, P_PRESET, "PRESET", npresets ? vg_itoa(sel_preset, t) : "-");
  draw_knob(1, P_VOLUME, "VOLUME", vg_ftoa(vibe_param_get(P_VOLUME), 2, t));
  draw_knob(2, P_OCTAVE, "OCTAVE", vg_itoa(g_octave_i, t));
  draw_knob(3, P_TRANSPOSE, "TRANSPOSE", vg_itoa(g_transpose_i, t));
  vg_text(vg_itoa(npresets, t), 470, 44, VG_SMALL, C_MUTED, VG_RIGHT);
  draw_softkeys((const char *const[]){ "PREV", "NEXT", "AUDIT", "FILES", "HELP", "ALL OFF" });
}

static void draw_files_page(void)
{
  int i;
  vg_text(cur_dir[0] ? cur_dir : "MEDIA SOURCES", 12, 40, VG_MED, C_ACC, VG_LEFT);
  for (i = 0; i < SF_UI_ROWS; i++) {
    int idx = top_file + i;
    int y = 68 + i * 26;
    if (idx >= nentries) break;
    if (idx == sel_file) vg_round(8, y, 464, 24, 4, C_ACC);
    vg_text(entries[idx][0] == 'd' || !cur_dir[0] ? ">" : ".sf2", 16, y + 4, VG_SMALL,
            idx == sel_file ? C_BG : C_BLUE, VG_LEFT);
    {
      const char *nm = cur_dir[0] ? field(entries[idx], 2) : entries[idx];
      char b[128];
      scpy(b, nm, sizeof b);
      for (char *c = b; *c && *c != '\t'; c++) if (*c == '\t') *c = ' ';
      vg_text(b, 58, y + 3, VG_MED, idx == sel_file ? C_BG : C_FG, VG_LEFT);
    }
    vf_button(200 + i, 8, y, 464, 24);
  }
  if (nentries == 0) vg_text("no media here - use the Editor to upload .sf2 files", 16, 100, VG_SMALL, C_MUTED, VG_LEFT);
  vg_text(status, 12, 282, VG_SMALL, C_GREEN, VG_LEFT);
  draw_softkeys((const char *const[]){ "BACK", "OPEN", "AUDIT", "SOUND", "HELP", "ALL OFF" });
}

static void draw_ctrl_page(void)
{
  char t[32];
  int i;
  vg_text("CONTROL", 12, 42, VG_BOLD, C_MUTED, VG_LEFT);
  vg_round(8, 64, 464, 48, 6, C_PANEL);
  vg_text("SOUNDFONT", 20, 70, VG_SMALL, C_MUTED, VG_LEFT);
  vg_text(font_name[0] ? font_name : "(none - demo voice)", 20, 86, VG_BIG, C_ACC, VG_LEFT);
  vg_text(vg_itoa(npresets, t), 462, 86, VG_MED, C_FG, VG_RIGHT);

  for (i = 0; i < 4; i++) {
    int y = 124 + i * 38;
    const char *label = i == 0 ? "PRESET" : i == 1 ? "VOLUME" : i == 2 ? "OCTAVE" : "TRANSPOSE";
    const char *value = i == 0 ? vg_itoa(sel_preset, t) :
                        i == 1 ? vg_ftoa(vibe_param_get(P_VOLUME), 2, t) :
                        i == 2 ? vg_itoa(g_octave_i, t) : vg_itoa(g_transpose_i, t);
    vg_round(8, y, 464, 32, 5, C_PANEL2);
    vg_text(label, 20, y + 8, VG_BOLD, C_MUTED, VG_LEFT);
    vg_text(value, 460, y + 5, VG_BIG, C_FG, VG_RIGHT);
    vf_encoder(VE_1 + i, 8, y, 464, 32);
  }
  vg_text(status, 12, 284, VG_SMALL, C_GREEN, VG_LEFT);
  draw_softkeys((const char *const[]){ "PREV", "NEXT", "AUDIT", "FILES", "HELP", "ALL OFF" });
}

void vibe_on_draw(uint16_t *fb)
{
  work();
  if (help) {
    vg_controls(ui_page == UI_FILES ? "FILES" : ui_page == UI_CTRL ? "CONTROL" : "SF2 PLAYER",
                HELP, (int)(sizeof HELP / sizeof HELP[0] / 2));
    return;
  }
  vg_begin(fb);
  vf_begin();
  vg_clear(C_BG);
  draw_tabs();
  if (ui_page == UI_FILES) draw_files_page();
  else if (ui_page == UI_CTRL) draw_ctrl_page();
  else draw_sound_page();
  vf_end(C_ACC);
}

/* ---- init ----------------------------------------------------------------- */

void vibe_on_init(float sample_rate)
{
  char path[VIBE_MEDIA_PATH];
  int pad, x, y;

  sr = sample_rate > 0.0f ? sample_rate : 48000.0f;
  vibe_notes(VIBE_NOTES | VIBE_NOTES_PADS | VIBE_NOTES_OWN_OCTAVE);
  vibe_keys_octave(2);
  vibe_params(PARAMS, P_COUNT);
  vibe_param_groups(GROUPS);
  vibe_editor_accent(C_ACC);
  vibe_editor_control(P_PRESET, VIBE_CTL_MENU);
  vf_mode(VF_ON_HOLD);
  vf_clicks(1);
  vf_enable(1);
  set_page(UI_SOUND);
  scpy(status, "no soundfont loaded - keys play a demo voice", sizeof status);
  ed_queue_status();

  while (vibe_media_dropped(path, sizeof path, &pad, &x, &y) > 0) {
    /* Enable drops; a file waiting here is picked up on the next MEDIA event. */
  }
  vibe_log("sf2-player: ready - load a .sf2 from FILES or the Editor");
}
