/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * bi-1 chip synth: the screen and the panel, laid out the FM-1 way.
 * Included by screen.c so it can share the band painter and key bookkeeping.
 *
 * Pages, each opened by its labelled button (press it again, or HOME, to come back):
 *   HOME  the parts: five lanes, keys play the selected part     HOME again: SONG (banks, songs, remix)
 *   SEQ   step editing: the 16 white keys are the 16 steps       FX    echo
 *   ENV   the part's envelope        LFO  the part's vibrato      ARP   the arpeggiator
 *   SEL   the chord progressions     EDIT the part's sound        GLO   the mix (white keys mute)
 * Everywhere: PLAY start / wind down / stop, REC record the keys into the loop, SAVE save the song,
 * SELECT choose the part, OCT- / OCT+ keyboard octave (SEQ: light / full pattern, SEL: chords A / B). */
static const uint16_t chip_col[CHIP_PARTS] = {0x7fbf /* lead */, 0xc53f /* arp */, 0x6c7f /* chords */, 0x3ff6 /* bass */, 0xfdcd /* drums */};
static const uint16_t chip_bank_col[CHIP_BANKS] = {0x7fbf, 0xc53f, 0xfb5a, 0x3ff6, 0xfdcd, 0x6c7f, 0xfe8f, 0xfc71};
static unsigned chip_sel = CHIP_LEAD;
enum { CHIP_LANE_Y = 34, CHIP_LANE_H = 30, CHIP_CELL_X = 74, CHIP_CELL_W = 10 };
static const char *const chip_level_names[3] = {"OFF", "LIGHT", "FULL"};

/* Panel lights: white keys 1-5 show which parts are in, PLAY and REC their state. */
static void chip_leds(void)
{
    static uint32_t shown = ~0U;
    uint32_t bits = 0;
    for (unsigned p = 0; p < CHIP_PARTS; ++p)
        if (chip_rt.live.lv[p] && !chip_song.mute[p]) bits |= 1U << (14 + white_keys[p]);
    if (chip_rt.playing) bits |= 1U << PLAY;
    if (chip_recording) bits |= 1U << REC;
    uint32_t changed = bits ^ shown;
    for (unsigned led = 0; led < 32; ++led)
        if (changed & (1U << led)) fm1_led_set(led, (bits >> led) & 1U);
    shown = bits;
}

static unsigned chip_cell_state(unsigned lane, unsigned step, unsigned lv, unsigned shown, unsigned here)
{
    unsigned on;
    if (lane == CHIP_DRUMS) on = chip_song.kit.drums[shown][step] != 0;
    else on = chip_song.kit.mel[lane][shown][step].d != 0;
    unsigned height = 0;
    if (on && lane != CHIP_DRUMS) height = (unsigned)(chip_song.kit.mel[lane][shown][step].o + 4) & 15U;
    if (on && lane == CHIP_DRUMS) { uint16_t m = chip_song.kit.drums[shown][step]; height = m & 1U ? 12 : m & 2U ? 9 : 5; }
    return 1U | (on << 1) | (here << 2) | ((lv != 0) << 3) | (height << 4) | ((chip_song.mute[lane] != 0) << 8);
}
static void chip_draw_main(void)
{
    /* header: the song's name, mood, key, tempo, progression */
    const struct chip_state *live = &chip_rt.live;
    uint32_t head = chip_revision * 7U + chip_song.bpm + (uint32_t)live->prog * 1000U + (uint32_t)chip_rt.song_mode * 3000U + (uint32_t)chip_recording * 9000U + (uint32_t)chip_drift * 20000U;
    if (cache->header_cache != head) {
        cache->header_cache = head;
        paint_fill(0, 0, 240, 30, BACK);
        text(6, 4, chip_song.name, 2, 0x7fbf, BACK);
        char line[28];
        memset(line, 0, sizeof line);
        strcpy(line, chip_mood_name()); strcat(line, " IN "); strcat(line, chip_key_name());
        text(6, 21, line, 1, TEXT, BACK);
        number(118, 21, chip_song.bpm, 3, 1, TEXT, BACK); text(138, 21, "BPM", 1, DIM, BACK);
        text(170, 21, live->prog ? "PROG B" : "PROG A", 1, 0xfb5a, BACK);
        if (chip_rt.song_mode) text(212, 21, "SONG", 1, GREEN, BACK);
        else if (chip_recording) text(216, 21, "REC", 1, 0xf800, BACK);
        else if (chip_drift) text(216, 21, chip_drift == 2 ? "DR+" : "DR", 1, AMBER, BACK);
        paint_fill(0, 30, 240, 1, PANEL);
    }
    /* five lanes: name, level and sound on the left, the sixteen steps on the right */
    unsigned step = chip_rt.playing ? (unsigned)((chip_rt.gstep + 15) & 15) : 99U;
    for (unsigned lane = 0; lane < CHIP_PARTS; ++lane) {
        unsigned lv = live->lv[lane], tg = chip_rt.has_target ? chip_rt.target.lv[lane] : lv;
        unsigned y = CHIP_LANE_Y + lane * CHIP_LANE_H;
        uint32_t row = lv | (tg << 2) | ((lane == chip_sel) << 4) | ((uint32_t)chip_song.patch[lane] << 5) | ((uint32_t)chip_song.mute[lane] << 7) |
                       ((uint32_t)chip_song.level[lane] << 8) | ((chip_revision & 0xffU) << 17);
        if (cache->row_cache[lane] != row) {
            cache->row_cache[lane] = row;
            paint_fill(0, y, CHIP_CELL_X - 2, CHIP_LANE_H - 2, BACK);
            uint16_t c = chip_song.mute[lane] ? DIM : chip_col[lane];
            if (lane == chip_sel) paint_fill(0, y + 1, 3, CHIP_LANE_H - 4, TEXT);
            text(6, y + 2, chip_part_name(lane), 1, c, BACK);
            /* a change waiting for the next bar shows in amber */
            text(6, y + 12, chip_song.mute[lane] ? "MUTE" : chip_level_names[tg != lv ? tg : lv], 1, tg != lv ? AMBER : lv ? TEXT : DIM, BACK);
            memset(cache->cell_cache[lane], 0xff, sizeof cache->cell_cache[lane]);
        }
        unsigned shown = lv ? (lv == CHIP_LIGHT ? 0U : 1U) : (tg ? (tg == CHIP_LIGHT ? 0U : 1U) : 1U);
        for (unsigned s = 0; s < CHIP_STEPS; ++s) {
            uint32_t state = chip_cell_state(lane, s, lv, shown, s == step);
            if (cache->cell_cache[lane][s] == state) continue;
            cache->cell_cache[lane][s] = state;
            unsigned x = CHIP_CELL_X + s * CHIP_CELL_W, top = y + 2, h = CHIP_LANE_H - 6;
            paint_fill(x, top, CHIP_CELL_W - 2, h, (s & 3U) == 0 ? 0x18c3 : 0x1082);
            if (s == step) paint_fill(x, top, CHIP_CELL_W - 2, h, 0x31a6);
            if (state & 2U) {
                unsigned hh = 4U + ((state >> 4) & 15U), yy = top + h - hh;
                uint16_t c = (state & 0x100U) ? DIM : (state & 8U) ? chip_col[lane] : 0x4208;
                if (s == step && (state & 8U)) c = TEXT;
                paint_fill(x, yy, CHIP_CELL_W - 2, hh, c);
            }
        }
    }
    return;
}
/* banks: filled when saved, outlined when playing or coming next */
static void chip_draw_banks(unsigned by)
{
    const struct chip_state *live = &chip_rt.live;
    for (unsigned b = 0; b < CHIP_BANKS; ++b) {
        unsigned used = chip_banks[b].used, now_b = live->bank == (int8_t)b, next_b = chip_rt.has_target && chip_rt.target.bank == (int8_t)b;
        uint32_t st = 1U | (used << 1) | (now_b << 2) | (next_b << 3);
        if (cache->tile_cache[b] == st) continue;
        cache->tile_cache[b] = st;
        unsigned x = 6 + b * 29;
        paint_fill(x, by, 26, 26, now_b ? TEXT : next_b ? AMBER : 0x2124);
        paint_fill(x + 2, by + 2, 22, 22, used ? chip_bank_col[b] : BACK);
        char n[2] = {(char)('1' + b), 0};
        text(x + 10, by + 10, n, 1, used ? BACK : DIM, used ? chip_bank_col[b] : BACK);
    }
}


enum { CP_MAIN, CP_SEQ, CP_FX, CP_ENV, CP_LFO, CP_ARP, CP_CHORDS, CP_SOUND, CP_MIX, CP_SONG };
static unsigned chip_page = CP_MAIN, chip_cursor, chip_version = 1, chip_piece, chip_prog_edit, chip_browse;
static int chip_last_degree = 4;
static const char *const chip_page_names[10] = {"", "STEP EDIT", "ECHO", "ENVELOPE", "VIBRATO", "ARPEGGIO", "CHORDS", "SOUND", "MIX", "SONG"};
static const uint8_t chip_drum_rows[7] = {0, 1, 2, 3, 4, 5, 8};
static const char *const chip_drum_names[7] = {"KICK", "SNARE", "CLAP", "HAT", "OPEN HAT", "TOM", "CRASH"};
static const char *const chip_roman[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};

static void chip_hint(void)
{
    static char sound_line[32];
    const char *hint = chip_notice;
    if (!hint) {
        memset(sound_line, 0, sizeof sound_line);
        strcpy(sound_line, chip_part_name(chip_sel)); strcat(sound_line, ": "); strcat(sound_line, chip_patch_name(chip_sel));
        strcat(sound_line, "  LEVEL "); char n[4]; unsigned lvl = chip_song.level[chip_sel];
        n[0] = (char)('0' + lvl / 100); n[1] = (char)('0' + lvl / 10 % 10); n[2] = (char)('0' + lvl % 10); n[3] = 0;
        strcat(sound_line, lvl >= 100 ? n : n + 1);
        hint = sound_line;
    }
    uint32_t h = (uint32_t)(uintptr_t)hint ^ (chip_sel << 24) ^ ((uint32_t)chip_song.patch[chip_sel] << 27) ^ ((uint32_t)chip_song.level[chip_sel] << 12);
    if (cache->hint_cache != h) {
        cache->hint_cache = h;
        paint_fill(0, 228, 240, 12, BACK);
        text(6, 231, hint, 1, chip_notice ? AMBER : DIM, BACK);
    }
}
static void chip_num(char out[8], int v)
{
    char t[8]; int n = 0, neg = v < 0; if (neg) v = -v;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 6);
    int k = 0; if (neg) out[k++] = '-';
    while (n) out[k++] = t[--n];
    out[k] = 0;
}
static void chip_knob_num(unsigned col, const char *label, int v, uint16_t c) { char b[8]; chip_num(b, v); knob(col, label, b, c); }
/* A page's title bar: page name and the part it edits. */
static void chip_page_header(const char *title, const char *sub, uint16_t color)
{
    uint32_t sig = signature(signature(color, title, (unsigned)strlen(title)), sub, (unsigned)strlen(sub)) ^ (chip_rt.playing << 3) ^ (chip_recording << 5);
    if (cache->header_cache == sig) return;
    cache->header_cache = sig;
    paint_fill(0, 0, 240, 30, BACK);
    text(6, 6, title, 2, color, BACK);
    text(6, 22, sub, 1, TEXT, BACK);
    if (chip_recording) text(216, 22, "REC", 1, 0xf800, BACK);
    else if (chip_rt.playing) text(210, 22, "PLAY", 1, GREEN, BACK);
    paint_fill(0, 30, 240, 1, PANEL);
}
static void chip_body_clear(uint32_t sig)
{
    if (cache->middle_cache == sig) return;
    cache->middle_cache = sig;
    paint_fill(0, 31, 240, 163, BACK);
    memset(cache->cell_cache, 0xff, sizeof cache->cell_cache);
    cache->title_cache = ~0U;
}

/* SEQ: the selected part's light or full pattern, sixteen steps on the sixteen white keys */
static void chip_draw_seq(void)
{
    unsigned part = chip_sel, which = chip_version;
    char sub[32]; memset(sub, 0, sizeof sub);
    strcpy(sub, chip_part_name(part)); strcat(sub, which ? "  FULL PATTERN" : "  LIGHT PATTERN");
    chip_page_header("STEP EDIT", sub, chip_col[part]);
    chip_body_clear(0x5e000000U | (part << 4) | which);
    unsigned step = chip_rt.playing ? (unsigned)((chip_rt.gstep + 15) & 15) : 99U;
    unsigned rows = part == CHIP_DRUMS ? 7U : 1U, top = 40, h = part == CHIP_DRUMS ? 20U : 140U;
    for (unsigned r = 0; r < rows; ++r) {
        if (part == CHIP_DRUMS && cache->row_cache[r] != (uint32_t)(r == chip_piece) + 7U) {
            cache->row_cache[r] = (uint32_t)(r == chip_piece) + 7U;
            paint_fill(0, top + r * h, 40, h - 2, BACK);
            text(2, top + r * h + 6, chip_drum_names[r], 1, r == chip_piece ? chip_col[CHIP_DRUMS] : DIM, BACK);
        }
        for (unsigned s2 = 0; s2 < 16; ++s2) {
            uint32_t st;
            int deg = 0; unsigned on, len = 0;
            if (part == CHIP_DRUMS) on = (chip_song.kit.drums[which][s2] >> chip_drum_rows[r]) & 1U;
            else { const struct chip_note *n = &chip_song.kit.mel[part][which][s2]; on = n->d != 0; deg = n->o; len = n->d; }
            st = 1U | (on << 1) | ((s2 == step) << 2) | ((s2 == chip_cursor) << 3) | ((uint32_t)(deg + 16) << 4) | (len << 10) | ((r == chip_piece) << 15);
            if (cache->cell_cache[r][s2] == st) continue;
            cache->cell_cache[r][s2] = st;
            unsigned x0 = part == CHIP_DRUMS ? 42U : 6U, cw = part == CHIP_DRUMS ? 12U : 14U, x = x0 + s2 * cw, y = top + r * h;
            paint_fill(x, y, cw - 2, h - 2, s2 == step ? 0x31a6 : (s2 & 3U) == 0 ? 0x18c3 : 0x1082);
            if (s2 == chip_cursor) { paint_fill(x, y + h - 4, cw - 2, 2, TEXT); }
            if (on) {
                uint16_t c = part == CHIP_DRUMS ? (r == chip_piece ? chip_col[CHIP_DRUMS] : 0x8410) : chip_col[part];
                if (part == CHIP_DRUMS) paint_fill(x + 1, y + 3, cw - 4, h - 9, c);
                else {
                    int yy = (int)(y + h - 14) - (deg + 7) * 4;
                    if (yy < (int)y + 2) yy = (int)y + 2;
                    paint_fill(x, (unsigned)yy, cw - 2, 6, c);
                }
            }
        }
    }
    const struct chip_note *cn = &chip_song.kit.mel[part < 4 ? part : 0][which][chip_cursor];
    char b[8]; chip_num(b, (int)chip_cursor + 1);
    knob(0, "STEP", b, TEXT);
    if (part == CHIP_DRUMS) { knob(1, "PIECE", chip_drum_names[chip_piece], chip_col[CHIP_DRUMS]); knob(2, "-", "", DIM); knob(3, "-", "", DIM); }
    else {
        if (cn->d) chip_knob_num(1, "K1 NOTE", cn->o, chip_col[part]); else knob(1, "K1 NOTE", "--", DIM);
        if (cn->d) chip_knob_num(2, "K2 LEN", cn->d, chip_col[part]); else knob(2, "K2 LEN", "--", DIM);
        knob(3, "K4 SHIFT", "ALL", DIM);
    }
}

/* the knob pages: a short description in the body, the four knobs along the bottom */
static void chip_draw_lines(uint32_t sig, const char *const *lines, unsigned n, const uint16_t *colors)
{
    if (cache->title_cache == sig) return;
    cache->title_cache = sig;
    paint_fill(0, 34, 240, 156, BACK);
    for (unsigned i = 0; i < n; ++i) text(10, 42 + i * 16, lines[i], 1, colors ? colors[i] : TEXT, BACK);
}
static void chip_draw_page(void)
{
    unsigned part = chip_sel;
    char sub[32]; memset(sub, 0, sizeof sub);
    strcpy(sub, chip_part_name(part)); strcat(sub, "  "); strcat(sub, chip_patch_name(part));
    uint32_t sig = chip_revision * 13U + chip_page * 7U + part + chip_prog_edit * 101U + chip_browse * 1009U;
    switch (chip_page) {
    case CP_FX: {
        chip_page_header("ECHO", "TRACKER-STYLE ECHO ON EVERY PART", 0x7fbf);
        chip_body_clear(0xec000000U);
        static const char *const times[3] = {"1/8", "1/8 D", "1/4"};
        static const char *const lines[3] = {"LEAD AND ARP SEND THE MOST,", "CHORDS A LITTLE, BASS NONE.", "K2 TIME FOLLOWS THE TEMPO."};
        chip_draw_lines(sig, lines, 3, 0);
        chip_knob_num(0, "K1 AMT", chip_song.echo, 0x7fbf);
        knob(1, "K2 TIME", times[chip_song.echo_time % 3], 0x7fbf);
        chip_knob_num(2, "K3 FDBK", chip_song.echo_fb, 0x7fbf);
        chip_knob_num(3, "K4 BPM", chip_song.bpm, TEXT);
        break; }
    case CP_ENV: case CP_LFO: {
        int env = chip_page == CP_ENV;
        chip_page_header(env ? "ENVELOPE" : "VIBRATO", sub, chip_col[part]);
        chip_body_clear(0xe0000000U | (env << 4) | part);
        if (part == CHIP_DRUMS) {
            static const char *const l[2] = {"THE DRUMS HAVE NO ENVELOPE OR", "VIBRATO: PICK A KIT ON EDIT."};
            chip_draw_lines(sig, l, 2, 0);
            for (unsigned k = 0; k < 4; ++k) knob(k, "-", "", DIM);
            break;
        }
        static const char *const le[3] = {"50 IS THE SOUND AS DESIGNED.", "LOWER IS SHORTER, HIGHER LONGER.", "SELECT CHOOSES THE PART."};
        static const char *const lv[3] = {"50 IS THE SOUND AS DESIGNED.", "DEPTH 0 TURNS VIBRATO OFF.", "SELECT CHOOSES THE PART."};
        chip_draw_lines(sig, env ? le : lv, 3, 0);
        if (env) {
            chip_knob_num(0, "K1 ATK", chip_song.env[part][0], chip_col[part]);
            chip_knob_num(1, "K2 DECAY", chip_song.env[part][1], chip_col[part]);
            chip_knob_num(2, "K3 SUST", chip_song.env[part][2], chip_col[part]);
            chip_knob_num(3, "K4 REL", chip_song.env[part][3], chip_col[part]);
        } else {
            chip_knob_num(0, "K1 RATE", chip_song.vib[part][0], chip_col[part]);
            chip_knob_num(1, "K2 DEPTH", chip_song.vib[part][1], chip_col[part]);
            chip_knob_num(2, "K3 DELAY", chip_song.vib[part][2], chip_col[part]);
            knob(3, "-", "", DIM);
        }
        break; }
    case CP_ARP: {
        static const char *const dirs[4] = {"UP", "DOWN", "UP-DN", "RAND"}, *const rates[3] = {"16TH", "8TH", "1/4"}, *const lens[3] = {"SHORT", "MED", "LONG"};
        chip_page_header("ARPEGGIO", "THE ARP PART RACES THE CHORDS", chip_col[CHIP_ARP]);
        chip_body_clear(0xa0000000U);
        static const char *const l[3] = {"THE ARP PLAYS THE BAR'S CHORD.", "K3 RANGE IS 1 TO 3 OCTAVES.", "LIGHT PLAYS AT HALF RATE."};
        chip_draw_lines(sig, l, 3, 0);
        knob(0, "K1 DIR", dirs[chip_song.arp_dir & 3], chip_col[CHIP_ARP]);
        knob(1, "K2 RATE", rates[chip_song.arp_rate % 3], chip_col[CHIP_ARP]);
        chip_knob_num(2, "K3 RANGE", chip_song.arp_range, chip_col[CHIP_ARP]);
        knob(3, "K4 LEN", lens[chip_song.arp_len % 3], chip_col[CHIP_ARP]);
        break; }
    case CP_CHORDS: {
        chip_page_header("CHORDS", chip_rt.live.prog ? "PLAYING B   ALGORITHM SWITCHES" : "PLAYING A   ALGORITHM SWITCHES", chip_col[CHIP_CHORDS]);
        chip_body_clear(0xc0000000U);
        if (cache->title_cache != sig) {
            cache->title_cache = sig;
            paint_fill(0, 34, 240, 156, BACK);
            for (unsigned pr = 0; pr < 2; ++pr) {
                unsigned y = 46 + pr * 64;
                text(10, y, pr ? "B" : "A", 2, pr == chip_prog_edit ? AMBER : DIM, BACK);
                for (unsigned c = 0; c < 4; ++c) {
                    unsigned x = 40 + c * 48;
                    paint_fill(x, y - 6, 42, 34, pr == chip_rt.live.prog ? 0x2124 : 0x1082);
                    text(x + 8, y + 4, chip_roman[chip_song.progs[pr][c] % 7], 2, pr == chip_prog_edit ? chip_col[CHIP_CHORDS] : TEXT, BACK);
                }
            }
            text(10, 176, "OCT EDITS A / B   SEL+GLO NEW", 1, DIM, BACK);
        }
        for (unsigned c = 0; c < 4; ++c) {
            static const char *const labels[4] = {"K1 BAR 1", "K2 BAR 2", "K3 BAR 3", "K4 BAR 4"};
            knob(c, labels[c], chip_roman[chip_song.progs[chip_prog_edit][c] % 7], chip_col[CHIP_CHORDS]);
        }
        break; }
    case CP_SOUND: {
        chip_page_header("SOUND", sub, chip_col[part]);
        chip_body_clear(0x50000000U | part);
        static const char *const names[CHIP_PARTS][3] = {{"PULSE 50", "PULSE 25", "SID LEAD"}, {"PULSE 12", "PULSE 25", "WAVE"},
            {"ARP CHORD", "PULSE PAD", "WAVE PAD"}, {"TRIANGLE", "PULSE BASS", "SID BASS"}, {"NES KIT", "GAME BOY KIT", "SID KIT"}};
        const char *l[3] = {names[part][0], names[part][1], names[part][2]};
        uint16_t c3[3]; for (unsigned k = 0; k < 3; ++k) c3[k] = k == chip_song.patch[part] % 3 ? chip_col[part] : DIM;
        chip_draw_lines(sig, l, 3, c3);
        knob(0, "K1 SOUND", chip_patch_name(part), chip_col[part]);
        chip_knob_num(1, "K2 LEVEL", chip_song.level[part], chip_col[part]);
        chip_knob_num(2, "K3 OCT", (int)chip_song.oct[part] - 3, chip_col[part]);
        knob(3, "K4 MUTE", chip_song.mute[part] ? "ON" : "OFF", chip_song.mute[part] ? AMBER : DIM);
        break; }
    case CP_MIX: {
        chip_page_header("MIX", "WHITE KEYS 1-5 MUTE THE PARTS", TEXT);
        chip_body_clear(0x3a000000U);
        if (cache->title_cache != sig) {
            cache->title_cache = sig;
            paint_fill(0, 34, 240, 156, BACK);
            for (unsigned l = 0; l < CHIP_PARTS; ++l) {
                unsigned x = 14 + l * 45, hh = (unsigned)chip_song.level[l] * 110U / 100U;
                paint_fill(x, 40, 30, 120, 0x1082);
                paint_fill(x, 40 + 120 - hh, 30, hh, chip_song.mute[l] ? DIM : chip_col[l]);
                text(x, 166, chip_part_name(l), 1, chip_song.mute[l] ? DIM : chip_col[l], BACK);
                text(x, 178, chip_song.mute[l] ? "MUTE" : "", 1, AMBER, BACK);
            }
        }
        chip_knob_num(0, "K1 LEAD", chip_song.level[0], chip_col[0]);
        chip_knob_num(1, "K2 ARP", chip_song.level[1], chip_col[1]);
        chip_knob_num(2, "K3 CHRD", chip_song.level[2], chip_col[2]);
        chip_knob_num(3, "K4 BASS", chip_song.level[3], chip_col[3]);
        break; }
    default: {   /* SONG */
        chip_page_header("SONG", chip_song.name, 0x7fbf);
        chip_body_clear(0x50a00000U);
        if (cache->title_cache != sig) {
            cache->title_cache = sig;
            paint_fill(0, 34, 240, 120, BACK);
            text(6, 38, "BANKS: KEY 1-8 PLAY, SAVE+KEY STORE", 1, DIM, BACK);
            static const char *const acts[8] = {"9 SONG", "10 DRIFT", "11 A/B", "12 LIFT", "13 NEW", "14 REMIX", "15 UNDO", "16 LOAD"};
            for (unsigned k = 0; k < 8; ++k) text(6 + (k % 4) * 58, 84 + (k / 4) * 12, acts[k], 1, k == 0 && chip_rt.song_mode ? GREEN : k == 1 && chip_drift ? AMBER : TEXT, BACK);
            text(6, 116, "SONG SLOT (PRESETS):", 1, DIM, BACK);
            char slot[24]; memset(slot, 0, sizeof slot); slot[0] = (char)('1' + chip_browse); slot[1] = ' '; strcat(slot, chip_store_name(chip_browse));
            text(6, 128, slot, 1, chip_browse == chip_slot ? GREEN : TEXT, BACK);
            text(6, 142, chip_browse == chip_slot ? "PLAYING NOW, AUTOSAVED" : "KEY 16 LOADS, SAVE STORES HERE", 1, DIM, BACK);
        }
        chip_draw_banks(52);
        static const char *const drifts[3] = {"OFF", "SLOW", "FAST"};
        knob(0, "DRIFT", drifts[chip_drift % 3], chip_drift ? AMBER : DIM);
        knob(1, "MOOD", chip_mood_name(), TEXT);
        knob(2, "KEY", chip_key_name(), TEXT);
        chip_knob_num(3, "K4 BPM", chip_song.bpm, TEXT);
        break; }
    }
}
static void chip_draw(void)
{
    if (chip_page == CP_MAIN) {
        chip_draw_main();
        static const char *const lvn[3] = {"OFF", "LIGHT", "FULL"};
        unsigned lv = chip_rt.has_target ? chip_rt.target.lv[chip_sel] : chip_rt.live.lv[chip_sel];
        chip_knob_num(0, "K1 LEVEL", chip_song.level[chip_sel], chip_col[chip_sel]);
        knob(1, "K2 PART", lvn[lv % 3], chip_col[chip_sel]);
        knob(2, "K3 KEY", chip_key_name(), TEXT);
        chip_knob_num(3, "K4 BPM", chip_song.bpm, TEXT);
    } else if (chip_page == CP_SEQ) chip_draw_seq();
    else chip_draw_page();
    chip_hint();
}

/* ------------------------------------------------------------ the panel */
static uint32_t chip_keys_held;
static uint8_t chip_bank_del_armed = 0xff;
static uint32_t chip_bank_del_at;
static void chip_open(unsigned pg)
{
    chip_page = chip_page == pg ? CP_MAIN : pg;
    cache->shown_page = ~0U;
    if (chip_page == CP_SONG) chip_browse = chip_slot;
}
static void chip_keyboard(const struct fm1_controls *controls, unsigned k, unsigned down)
{
    uint32_t bit = 1U << k;
    if (down) {
        int note = 53 + (int)k + 12 * ((int)chip_song.oct[chip_sel] - 3);   /* F3 + key, moved by the part's octave */
        if (note < 12) note = 12;
        if (note > 120) note = 120;
        chip_key_down(chip_sel, k, (unsigned)note);
        chip_record_down(chip_sel, k, (unsigned)note);
        chip_keys_held |= bit;
    } else if (chip_keys_held & bit) {
        chip_key_up(k); chip_record_up(k);
        chip_keys_held &= ~bit;
    }
    (void)controls;
}
static void chip_page_input(const struct fm1_controls *controls, uint32_t pressed, uint32_t old_keys, uint32_t now)
{
    const int16_t *e = controls->encoders;
    uint32_t held = controls->buttons;
    unsigned save = (held & BUTTON(SAVE)) != 0, glo_held = (held & BUTTON(GLO)) != 0;
    /* buttons that open pages */
    if (pressed & BUTTON(HOME)) { if (chip_page == CP_MAIN) chip_open(CP_SONG); else chip_open(chip_page); }
    if (pressed & BUTTON(SEQ)) chip_open(CP_SEQ);
    if (pressed & BUTTON(FX)) chip_open(CP_FX);
    if (pressed & BUTTON(ENV)) chip_open(CP_ENV);
    if (pressed & BUTTON(LFO)) chip_open(CP_LFO);
    if (pressed & BUTTON(ARP)) chip_open(CP_ARP);
    if ((pressed & BUTTON(SCL)) && !glo_held) chip_open(CP_CHORDS);
    if ((pressed & BUTTON(SCL)) && glo_held) chip_new_progressions();
    if (pressed & BUTTON(EDIT)) chip_open(CP_SOUND);
    if ((pressed & BUTTON(GLO)) && !(held & BUTTON(SCL))) chip_open(CP_MIX);
    /* transport */
    if (pressed & BUTTON(PLAY)) chip_play_button();
    if (pressed & BUTTON(REC)) chip_record_toggle();
    if ((pressed & BUTTON(SAVE)) && chip_page != CP_SONG) { chip_notice = chip_store_save(chip_slot) ? "SAVE FAILED" : "SONG SAVED"; chip_notice_until = 0; ++chip_revision; }
    if ((pressed & BUTTON(SAVE)) && chip_page == CP_SONG && !controls->keys) {
        chip_notice = chip_store_save(chip_browse) ? "SAVE FAILED" : "SONG SAVED IN THIS SLOT"; chip_notice_until = 0; ++chip_revision; cache->title_cache = ~0U;
    }
    /* SELECT always chooses the part */
    if (e[0]) { chip_sel = (unsigned)((int)chip_sel + (e[0] > 0 ? 1 : -1) + CHIP_PARTS) % CHIP_PARTS; chip_piece = 0; ++chip_revision; cache->shown_page = chip_page == CP_SEQ ? ~0U : cache->shown_page; }
    int d1 = e[2] > 0 ? 1 : e[2] < 0 ? -1 : 0, d2 = e[3] > 0 ? 1 : e[3] < 0 ? -1 : 0, d3 = e[4] > 0 ? 1 : e[4] < 0 ? -1 : 0, d4 = e[5] > 0 ? 1 : e[5] < 0 ? -1 : 0;
    int dp = e[6] > 0 ? 1 : e[6] < 0 ? -1 : 0, da = e[1] > 0 ? 1 : e[1] < 0 ? -1 : 0;
    int oct = (pressed & BUTTON(OCT_UP)) ? 1 : (pressed & BUTTON(OCT_DOWN)) ? -1 : 0;
    switch (chip_page) {
    case CP_MAIN:
        if (dp) chip_patch(chip_sel, dp);                                  /* PRESETS: the part's sound */
        if (da) chip_mood(da);                                              /* ALGORITHM: mood */
        if (d1) chip_level(chip_sel, d1);
        if (d2) {
            struct chip_state n = chip_rt.has_target ? chip_rt.target : chip_rt.live;
            int lv = (int)n.lv[chip_sel] + d2;
            if (lv >= 0 && lv <= 2) while ((chip_rt.has_target ? chip_rt.target : chip_rt.live).lv[chip_sel] != (unsigned)lv) chip_cycle_part(chip_sel);
        }
        if (d3) chip_key(d3);
        if (e[5]) chip_tempo(fm_encoder_delta(5, e[5]));
        if (oct) { chip_song.oct[chip_sel] = (uint8_t)((int)chip_song.oct[chip_sel] + oct < 0 ? 0 : (int)chip_song.oct[chip_sel] + oct > 6 ? 6 : (int)chip_song.oct[chip_sel] + oct); ++chip_revision; }
        break;
    case CP_SEQ: {
        unsigned part = chip_sel;
        if (dp) chip_cursor = (chip_cursor + 16U + (unsigned)dp) & 15U, ++chip_revision;
        if (oct) { chip_version = oct > 0 ? 1U : 0U; cache->shown_page = ~0U; }
        if (part == CHIP_DRUMS) { if (d1 || d3 || da) { int dd = d1 ? d1 : d3 ? d3 : da; chip_piece = (chip_piece + 7U + (unsigned)dd) % 7U; ++chip_revision; } }
        else {
            if (d1) { chip_step_pitch(part, chip_version, chip_cursor, d1); chip_last_degree = chip_song.kit.mel[part][chip_version][chip_cursor].o; }
            if (d2) chip_step_length(part, chip_version, chip_cursor, d2);
            if (d4) for (unsigned i = 0; i < 16; ++i) if (chip_song.kit.mel[part][chip_version][i].d) chip_step_pitch(part, chip_version, i, d4);
        }
        break; }
    case CP_FX:
        if (d1) chip_echo(d1);
        if (d2) chip_echo_time(d2);
        if (d3) chip_echo_feedback(d3);
        if (e[5]) chip_tempo(fm_encoder_delta(5, e[5]));
        if (dp) chip_patch(chip_sel, dp);
        break;
    case CP_ENV:
        if (d1) chip_env_edit(chip_sel, 0, d1); if (d2) chip_env_edit(chip_sel, 1, d2);
        if (d3) chip_env_edit(chip_sel, 2, d3); if (d4) chip_env_edit(chip_sel, 3, d4);
        if (dp) chip_patch(chip_sel, dp);
        break;
    case CP_LFO:
        if (d1) chip_vib_edit(chip_sel, 0, d1); if (d2) chip_vib_edit(chip_sel, 1, d2); if (d3) chip_vib_edit(chip_sel, 2, d3);
        if (dp) chip_patch(chip_sel, dp);
        break;
    case CP_ARP:
        if (d1) chip_arp_param(0, d1); if (d2) chip_arp_param(1, d2); if (d3) chip_arp_param(2, d3); if (d4) chip_arp_param(3, d4);
        break;
    case CP_CHORDS:
        if (oct) { chip_prog_edit = oct > 0 ? 1U : 0U; ++chip_revision; }
        if (d1) chip_prog_chord(chip_prog_edit, 0, d1); if (d2) chip_prog_chord(chip_prog_edit, 1, d2);
        if (d3) chip_prog_chord(chip_prog_edit, 2, d3); if (d4) chip_prog_chord(chip_prog_edit, 3, d4);
        if (da) chip_toggle_prog();                                       /* ALGORITHM: which progression plays */
        break;
    case CP_SOUND:
        if (d1 || dp) chip_patch(chip_sel, d1 ? d1 : dp);
        if (d2) chip_level(chip_sel, d2);
        if (d3 || oct) { int o = (int)chip_song.oct[chip_sel] + (d3 ? d3 : oct); chip_song.oct[chip_sel] = (uint8_t)(o < 0 ? 0 : o > 6 ? 6 : o); ++chip_revision; }
        if (d4) chip_mute(chip_sel);
        break;
    case CP_MIX:
        if (d1) chip_level(0, d1); if (d2) chip_level(1, d2); if (d3) chip_level(2, d3); if (d4) chip_level(3, d4);
        if (dp) chip_level(4, dp);
        break;
    default:   /* SONG */
        if (dp) { chip_browse = (chip_browse + 8U + (unsigned)dp) & 7U; ++chip_revision; }
        if (da) chip_mood(da);
        if (d1) chip_drift_cycle();
        if (d3) chip_key(d3);
        if (e[5]) chip_tempo(fm_encoder_delta(5, e[5]));
        break;
    }
    /* the keys */
    for (unsigned k = 0; k < 27; ++k) {
        uint32_t bit = 1U << k;
        unsigned down = (controls->keys & bit) && !(old_keys & bit), up = !(controls->keys & bit) && (old_keys & bit);
        if (!down && !up) continue;
        int white = -1, black = -1;
        for (unsigned i = 0; i < 16; ++i) if (white_keys[i] == k) white = (int)i;
        for (unsigned i = 0; i < 10; ++i) if (edit_keys[i] == k) black = (int)i;
        if (chip_page == CP_SEQ && down && white >= 0) {             /* the white keys are the sixteen steps */
            chip_cursor = (unsigned)white;
            chip_step_toggle(chip_sel, chip_version, (unsigned)white, chip_last_degree, chip_drum_rows[chip_piece]);
            if (chip_sel != CHIP_DRUMS && chip_song.kit.mel[chip_sel][chip_version][white].d) chip_live_note(chip_sel, (unsigned)(chip_last_degree + 7) % 14);
            continue;
        }
        if (chip_page == CP_SEQ && white >= 0) continue;
        if (chip_page == CP_SEQ && down && black >= 0 && black < 3) {     /* black keys 1-3: clear, new pattern, copy */
            if (black == 0) chip_clear_part(chip_sel);
            else if (black == 1) chip_new_patterns(chip_sel);
            else chip_pattern_copy(chip_sel, chip_version);
            continue;
        }
        if (chip_page == CP_SEQ && black >= 0 && black < 3) continue;
        if (chip_page == CP_MIX && white >= 0) { if (down && white < CHIP_PARTS) chip_mute((unsigned)white); continue; }
        if (chip_page == CP_SONG && white >= 0) {                     /* banks and song moves */
            if (!down) continue;
            if (white < 8) { if (save) chip_bank_save((unsigned)white); else chip_bank_play((unsigned)white); }
            else switch (white) {
                case 8: chip_song_mode(); break;
                case 9: chip_drift_cycle(); break;
                case 10: chip_toggle_prog(); break;
                case 11: chip_toggle_lift(); break;
                case 12: chip_new_loop(); break;
                case 13: chip_remix(); break;
                case 14: (void)chip_undo(); break;
                default:
                    if (chip_browse == chip_slot) { chip_notice = "THIS SONG IS ALREADY PLAYING"; chip_notice_until = 0; }
                    else { (void)chip_store_save(chip_slot); chip_notice = chip_store_load(chip_browse) ? "THAT SLOT IS EMPTY" : "SONG LOADED"; chip_notice_until = 0; }
                    ++chip_revision; cache->title_cache = ~0U;
                    break;
            }
            cache->title_cache = ~0U;
            continue;
        }
        if (chip_page == CP_SONG && black >= 0 && black < 8) {          /* black key n twice: delete bank n */
            if (!down) continue;
            if (chip_bank_del_armed == black && now - chip_bank_del_at < 1500U) { chip_bank_delete((unsigned)black); chip_bank_del_armed = 0xff; }
            else { chip_bank_del_armed = (uint8_t)black; chip_bank_del_at = now; chip_notice = "PRESS AGAIN TO DELETE THAT BANK"; chip_notice_until = 0; ++chip_revision; }
            continue;
        }
        chip_keyboard(controls, k, down);
    }
    blocked_keys |= controls->keys;
}
