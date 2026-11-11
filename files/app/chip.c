/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * bi-1 chip synth: the composer and the controls (control task).
 * The song generator, moods, progressions, light/full patterns, arpeggiator,
 * banks and song mode follow the bi-1 web app. */
#include <string.h>
#include "chip.h"
#include "chip_dsp.h"
#include <stddef.h>

struct chip_song chip_song;
struct chip_bank chip_banks[CHIP_BANKS];
struct chip_runtime chip_rt;
const char *chip_notice;
uint32_t chip_notice_until, chip_revision;

/* ------------------------------------------------------------ randomness */
static uint32_t rng = 0x2545f491u;
static uint32_t rnd(void) { uint32_t x = rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rng = x ? x : 1u; return rng; }
static int rr(int n) { return n > 0 ? (int)(rnd() % (unsigned)n) : 0; }
static int chance(int percent) { return rr(100) < percent; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
#define PICK(a) ((a)[rr((int)(sizeof(a) / sizeof((a)[0])))])

/* ------------------------------------------------------------ the app's world */
static const char *const mood_names[CHIP_MOODS] = {"HERO", "QUEST", "CASTLE", "DUNGEON", "BOSS"};
static const uint8_t mood_bpm[CHIP_MOODS][2] = {{132, 148}, {120, 136}, {110, 126}, {100, 116}, {140, 156}};
static const char *const key_names[12] = {"C", "C#", "D", "EB", "E", "F", "F#", "G", "AB", "A", "BB", "B"};
static const char *const part_names[CHIP_PARTS] = {"LEAD", "ARP", "CHORDS", "BASS", "DRUMS"};
static const char *const patch_names[CHIP_PARTS][3] = {
    {"PULSE 50", "PULSE 25", "SID LEAD"}, {"PULSE 12", "PULSE 25", "WAVE"}, {"ARP CHORD", "PULSE PAD", "WAVE PAD"},
    {"TRIANGLE", "PULSE BASS", "SID BASS"}, {"NES", "GAME BOY", "SID"}};
static const uint8_t progs[7][4] = {{0, 5, 2, 6}, {0, 5, 3, 4}, {5, 3, 0, 4}, {0, 2, 5, 6}, {0, 6, 5, 6}, {5, 6, 0, 4}, {0, 3, 5, 4}};
static const char *const w1[] = {"PIXEL", "TURBO", "MEGA", "SUPER", "COSMIC", "CRYSTAL", "DRAGON", "STAR", "RETRO", "HYPER", "LUNAR", "TINY"};
static const char *const w2[] = {"QUEST", "CASTLE", "RUNNER", "DUNGEON", "ISLAND", "GALAXY", "TEMPLE", "ZONE", "FORTRESS", "HERO", "RUSH", "LEGEND"};
static const char *const arp_dirs[4] = {"UP", "DOWN", "UP-DOWN", "RANDOM"};
static const char *const arp_rates[3] = {"16THS", "8THS", "QUARTERS"};

static void say(const char *t) { chip_notice = t; chip_notice_until = 0; ++chip_revision; }
/* Each part's chosen sound, rebuilt with its ENV and LFO edits. */
static const uint8_t sound_ids[4][3] = {{P_PULSE50, P_PULSE25, P_SIDLEAD}, {P_PULSE12, P_PULSE25, P_WAVE}, {P_ARPCHORD, P_PULSEPAD, P_WAVEPAD}, {P_TRI, P_PULSEBASS, P_SIDBASS}};
void chip_refresh_sounds(void)
{
    for (unsigned p = 0; p < 4; ++p) chip_dsp_part_patch(p, sound_ids[p][chip_song.patch[p] % 3]);
}
static uint32_t now_ms, remix_at;
static int32_t end_stop_at = -1;
static void stop_now(void);
static void drift_step(void);
static void store_boot(void);

/* ------------------------------------------------------------ patterns */
static int nearest_chord_tone(int o)
{
    static const int tones[7] = {-3, 0, 2, 4, 7, 9, 11};
    int best = 0;
    for (int i = 0; i < 7; ++i) { int a = tones[i] - o, b = best - o; if ((a < 0 ? -a : a) < (b < 0 ? -b : b)) best = tones[i]; }
    return best;
}
static void gen_line(struct chip_note p[CHIP_STEPS], int dense)
{
    static const int dpool[] = {0, 2, 3, 4, 6, 8, 10, 11, 12, 14}, lpool[] = {0, 3, 4, 6, 8, 12, 14};
    memset(p, 0, sizeof(struct chip_note) * CHIP_STEPS);
    uint32_t on = 1U;
    int want = dense ? 6 + rr(3) : 3 + rr(2), guard = 0, count = 1;
    while (count < want && guard++ < 100) { int s = dense ? PICK(dpool) : PICK(lpool); if (!(on & (1U << s))) { on |= 1U << s; ++count; } }
    static const int moves[] = {-2, -1, 1, 1, 2, 3, -3}, starts[] = {0, 2, 4, 7};
    int o = PICK(starts), first = 1;
    for (int s = 0; s < CHIP_STEPS; ++s) {
        if (!(on & (1U << s))) continue;
        int next = 16;
        for (int k = s + 1; k < CHIP_STEPS; ++k) if (on & (1U << k)) { next = k; break; }
        if (!first) { o += PICK(moves); if (s % 4 == 0 && chance(60)) o = nearest_chord_tone(o); o = clampi(o, -2, 11); }
        first = 0;
        p[s].o = (int8_t)o; p[s].d = (uint8_t)clampi(next - s, 1, 4);
    }
}
static void gen_chords(struct chip_note p[CHIP_STEPS], int full)
{
    static const int8_t shapes[4][5] = {{0, 3, 6, 10, 12}, {0, 6, 8, 14, -1}, {0, 3, 8, 11, -1}, {0, 4, 8, 12, -1}};
    memset(p, 0, sizeof(struct chip_note) * CHIP_STEPS);
    if (!full) { p[0].d = 16; return; }
    const int8_t *s = shapes[rr(4)];
    for (int i = 0; i < 5 && s[i] >= 0; ++i) p[s[i]].d = 2;
}
static void gen_bass(struct chip_note p[CHIP_STEPS], int full)
{
    memset(p, 0, sizeof(struct chip_note) * CHIP_STEPS);
    if (full) { for (int s = 0; s < 16; s += 2) { p[s].o = (int8_t)(s % 4 == 2 ? 7 : 0); p[s].d = 2; } return; }
    p[0].d = 6; p[8].d = 5;
    if (chance(60)) { p[14].o = (int8_t)(chance(50) ? 4 : 2); p[14].d = 2; }
}
static void gen_drums(uint16_t p[CHIP_STEPS], int full)
{
    memset(p, 0, sizeof(uint16_t) * CHIP_STEPS);
#define SET(pc, s) (p[s] |= (uint16_t)(1U << (pc)))
    if (!full) {
        SET(0, 0); SET(0, 8); SET(3, 2); SET(3, 6); SET(3, 10); SET(3, 14);
        if (chance(50)) SET(1, 12);
        return;
    }
    static const int8_t kicks[3][5] = {{0, 4, 8, 12, -1}, {0, 6, 8, 14, -1}, {0, 4, 8, 10, 12}};
    const int8_t *k = kicks[rr(3)];
    for (int i = 0; i < 5 && k[i] >= 0; ++i) SET(0, k[i]);
    SET(1, 4); SET(1, 12);
    static const int hats[12] = {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15};
    for (int i = 0; i < 12; ++i) if (chance(55)) SET(3, hats[i]);
    if (chance(60)) SET(4, 6);
    if (chance(60)) SET(4, 14);
    if (chance(40)) SET(2, 12);
#undef SET
}
static uint8_t arp_random[8];
static void build_arp(void)
{
    int up[16], n = 0, range = clampi(chip_song.arp_range, 1, 3);
    for (int o = 0; o < range; ++o) { up[n++] = 7 * o; up[n++] = 7 * o + 2; up[n++] = 7 * o + 4; }
    up[n++] = 7 * range;
    int seq[32], len = 0;
    switch (chip_song.arp_dir & 3) {
    case 1: for (int i = n - 1; i >= 0; --i) seq[len++] = up[i]; break;
    case 2: for (int i = 0; i < n; ++i) seq[len++] = up[i]; for (int i = n - 2; i >= 1; --i) seq[len++] = up[i]; break;
    case 3: for (int i = 0; i < 8; ++i) seq[len++] = up[arp_random[i] % n]; break;
    default: for (int i = 0; i < n; ++i) seq[len++] = up[i]; break;
    }
    static const int gaps[3] = {1, 2, 4};
    int sp = gaps[chip_song.arp_rate % 3];
    for (int which = 0; which < 2; ++which) {
        int gap = which ? sp : (sp * 2 > 4 ? 4 : sp * 2);
        struct chip_note *p = chip_song.kit.mel[CHIP_ARP][which];
        memset(p, 0, sizeof(struct chip_note) * CHIP_STEPS);
        for (int s = 0, i = 0; s < 16; s += gap, ++i) { p[s].o = (int8_t)seq[i % len]; p[s].d = (uint8_t)gap; }
    }
}
static void gen_part(unsigned part)
{
    struct chip_kit *k = &chip_song.kit;
    switch (part) {
    case CHIP_LEAD: gen_line(k->mel[CHIP_LEAD][0], 0); gen_line(k->mel[CHIP_LEAD][1], 1); break;
    case CHIP_ARP: for (int i = 0; i < 8; ++i) arp_random[i] = (uint8_t)rr(16); build_arp(); break;
    case CHIP_CHORDS: gen_chords(k->mel[CHIP_CHORDS][0], 0); gen_chords(k->mel[CHIP_CHORDS][1], 1); break;
    case CHIP_BASS: gen_bass(k->mel[CHIP_BASS][0], 0); gen_bass(k->mel[CHIP_BASS][1], 1); break;
    default: gen_drums(k->drums[0], 0); gen_drums(k->drums[1], 1); break;
    }
}
static void new_progs(void)
{
    int a = rr(7), b = rr(7);
    while (b == a) b = rr(7);
    memcpy(chip_song.progs[0], progs[a], 4); memcpy(chip_song.progs[1], progs[b], 4);
}
static void new_project(unsigned mood)
{
    struct chip_song *s = &chip_song;
    memset(s->name, 0, sizeof s->name);
    strcpy(s->name, PICK(w1)); strcat(s->name, " "); strcat(s->name, PICK(w2));
    s->mood = (uint8_t)(mood % CHIP_MOODS);
    static const uint8_t keys[] = {9, 4, 2, 7, 0, 5, 11};
    s->key = PICK(keys);
    s->bpm = (uint8_t)(mood_bpm[s->mood][0] + rr(mood_bpm[s->mood][1] - mood_bpm[s->mood][0] + 1));
    new_progs();
    static const uint8_t p_drums[] = {0, 1, 2}, p_bass[] = {0, 0, 1, 2}, p_chords[] = {0, 1, 2}, p_arp[] = {0, 1, 2}, p_lead[] = {0, 0, 1, 2};
    s->patch[CHIP_DRUMS] = PICK(p_drums); s->patch[CHIP_BASS] = PICK(p_bass); s->patch[CHIP_CHORDS] = PICK(p_chords);
    s->patch[CHIP_ARP] = PICK(p_arp); s->patch[CHIP_LEAD] = PICK(p_lead);
    static const uint8_t levels[CHIP_PARTS] = {70, 55, 55, 80, 75};
    memcpy(s->level, levels, sizeof levels);
    memset(s->mute, 0, sizeof s->mute);
    s->arp_dir = 0; s->arp_rate = 0; s->arp_range = 2; s->arp_len = 1;
    if (!s->echo) { s->echo = 5; s->echo_time = 1; s->echo_fb = 3; }
    memset(s->env, 50, sizeof s->env); memset(s->vib, 50, sizeof s->vib);
    static const uint8_t octs[CHIP_PARTS] = {4, 4, 3, 2, 3};
    memcpy(s->oct, octs, sizeof octs);
    for (unsigned p = 0; p < CHIP_PARTS; ++p) gen_part(p);
    chip_dsp_tempo(s->bpm);
    chip_dsp_mix();
    chip_refresh_sounds();
}

/* ------------------------------------------------------------ state changes land on the next bar */
static struct chip_state pending(void) { return chip_rt.has_target ? chip_rt.target : chip_rt.live; }
static void request(struct chip_state next)
{
    if (!chip_rt.playing) {
        chip_rt.has_target = 0;
        if (next.bank >= 0 && next.bank < CHIP_BANKS && chip_banks[next.bank].used && next.bank != chip_rt.live.bank)
            chip_song.kit = chip_banks[next.bank].kit;
        chip_rt.live = next;
        ++chip_revision;
        return;
    }
    chip_rt.has_target = 0;
    chip_rt.target = next;
    int32_t gs = chip_rt.gstep, at = chip_rt.change_at;
    if (at < 0 || at <= gs + 1) { at = (gs + 15) & ~15; if (at - gs < 4) at += 16; }
    chip_rt.change_at = at;
    chip_rt.has_target = 1;
    ++chip_revision;
}

/* ------------------------------------------------------------ undo for remix */
static struct { uint8_t valid; struct chip_song song; struct chip_bank banks[CHIP_BANKS]; } undo;

/* ------------------------------------------------------------ API */
void chip_init(uint32_t seed)
{
    rng ^= seed * 2654435761u; rnd();
    chip_dsp_init();
    memset(&chip_rt, 0, sizeof chip_rt);
    chip_rt.change_at = -1; chip_rt.mood_next = -1; chip_rt.key_next = -1;
    chip_rt.live.bank = -1; chip_rt.target.bank = -1;
    static const uint8_t moods[] = {0, 0, 1, 1, 2, 3, 4};
    chip_song.echo = 5; chip_song.echo_time = 1; chip_song.echo_fb = 3;
    new_project(PICK(moods));
    static const uint8_t start[CHIP_PARTS] = {CHIP_FULL, CHIP_LIGHT, CHIP_FULL, CHIP_FULL, CHIP_FULL};
    memcpy(chip_rt.live.lv, start, sizeof start);
    memset(chip_banks, 0, sizeof chip_banks);
    say(chip_song.name);
    store_boot();
}
static void store_poll(uint32_t now);
void chip_poll(uint32_t now)
{
    store_poll(now);
    static int32_t last_bar = -1;
    now_ms = now;
    if (end_stop_at >= 0 && chip_rt.gstep >= end_stop_at) { stop_now(); say("STOPPED"); }
    if (chip_rt.playing) {
        int32_t bar = (chip_rt.gstep - 1) >> 4;
        if (bar != last_bar && ((chip_rt.gstep - 1) & 15) >= 8) {
            last_bar = bar;
            int every = chip_drift == 2 ? 4 : 8;
            if (chip_drift && bar % every == every - 1) drift_step();
        }
    }
    if (chip_notice && !chip_notice_until) chip_notice_until = now + 2200U;
    if (chip_notice && (int32_t)(now - chip_notice_until) > 0) { chip_notice = 0; ++chip_revision; }
}
void chip_play(int on)
{
    if (on) {
        chip_rt.gstep = (chip_rt.gstep + 15) & ~15;
        if (chip_rt.has_target) chip_rt.change_at = chip_rt.gstep;
        chip_dsp_restart_clock();
        chip_rt.playing = 1;
    } else {
        chip_rt.playing = 0; chip_rt.song_mode = 0; chip_winding = 0; end_stop_at = -1; chip_recording = 0;
        chip_dsp_stop();
    }
    ++chip_revision;
}
uint8_t chip_drift, chip_recording, chip_winding;
static struct chip_state pre_end;
static void stop_now(void)
{
    if (chip_winding) { chip_rt.has_target = 0; chip_rt.live = pre_end; }
    chip_winding = 0; end_stop_at = -1;
    chip_play(0);
}
void chip_play_button(void)
{
    struct chip_state p = pending();
    int any = 0;
    for (unsigned l = 0; l < CHIP_PARTS; ++l) any |= p.lv[l] != 0;
    if (!chip_rt.playing) {
        if (!any) { p.lv[CHIP_ARP] = CHIP_FULL; chip_rt.live = p; }
        chip_play(1);
        return;
    }
    if (chip_rt.song_mode || chip_winding || !any) { stop_now(); say("STOPPED"); return; }
    pre_end = p;
    struct chip_state off = p;
    for (unsigned l = 0; l < CHIP_PARTS; ++l) off.lv[l] = 0;
    request(off);
    end_stop_at = chip_rt.change_at + 16;
    chip_winding = 1;
    say("WINDING DOWN: PLAY AGAIN STOPS NOW");
}
void chip_clear_part(unsigned part)
{
    if (part >= CHIP_PARTS) return;
    struct chip_state p = pending();
    unsigned which = p.lv[part] == CHIP_LIGHT ? 0 : 1;
    if (part == CHIP_DRUMS) memset(chip_song.kit.drums[which], 0, sizeof chip_song.kit.drums[which]);
    else memset(chip_song.kit.mel[part][which], 0, sizeof chip_song.kit.mel[part][which]);
    ++chip_revision;
    say("STEPS CLEARED");
}
/* ------------------------------------------------------------ drift, as in the app: every 8 bars (or 4) the song changes itself */
void chip_drift_cycle(void)
{
    chip_drift = (uint8_t)((chip_drift + 1) % 3);
    static const char *const names[3] = {"DRIFT OFF", "DRIFT SLOW: EVERY 8 BARS", "DRIFT FAST: EVERY 4 BARS"};
    say(names[chip_drift]);
    if (chip_drift && !chip_rt.playing) chip_play(1);
}
static void mutate_part(unsigned l, int wild)
{
    struct chip_kit *k = &chip_song.kit;
    if (l == CHIP_DRUMS) {
        static const int pcs[5] = {0, 1, 3, 3, 4};
        int pc = pcs[rr(5)];
        for (int n = 0; n < (wild ? 2 : 1); ++n) { int st = rr(16); if (pc == 0 && st == 0) continue; k->drums[1][st] ^= (uint16_t)(1U << pc); }
        say("DRIFT: THE DRUMS SHIFT"); return;
    }
    if (l == CHIP_ARP && chance(40)) { chip_arp_style(1); say("DRIFT: THE ARP TURNS"); return; }
    struct chip_note *pat = k->mel[l][1];
    int idx[16], n = 0;
    for (int i = 0; i < 16; ++i) if (pat[i].d) idx[n++] = i;
    if (n && chance(60)) { int i = idx[rr(n)]; static const int small[2] = {-1, 1}, big[4] = {-2, -1, 1, 2}; pat[i].o = (int8_t)clampi(pat[i].o + (wild ? big[rr(4)] : small[rr(2)]), -3, 13); say("DRIFT: A NOTE MOVES"); }
    else {
        int i = rr(16);
        if (pat[i].d && n > 2) { pat[i].d = 0; say("DRIFT: A NOTE DROPS"); }
        else { static const int arp_o[4] = {0, 2, 4, 7}, lead_o[3] = {0, 2, 4}; pat[i].o = (int8_t)(l == CHIP_ARP ? arp_o[i % 4] : l == CHIP_LEAD ? lead_o[rr(3)] : 0); pat[i].d = (uint8_t)(l == CHIP_ARP ? 1 : 2); say("DRIFT: A NOTE ARRIVES"); }
    }
    ++chip_revision;
}
static void drift_step(void)
{
    if (!chip_rt.playing || chip_rt.song_mode || !chip_drift || chip_winding) return;
    int wild = chip_drift == 2;
    struct chip_state s0 = pending();
    int on[CHIP_PARTS], off[CHIP_PARTS], non = 0, noff = 0, banks[CHIP_BANKS], nb = 0;
    for (int l = 0; l < CHIP_PARTS; ++l) { if (s0.lv[l]) on[non++] = l; else off[noff++] = l; }
    for (int b = 0; b < CHIP_BANKS; ++b) if (chip_banks[b].used) banks[nb++] = b;
    int r = rr(100);
    if (r < 32) {
        if (non <= 2 || (noff && chance(50))) { if (noff) { s0.lv[off[rr(noff)]] = CHIP_FULL; say("DRIFT: A PART DRIFTS IN"); } }
        else { s0.lv[on[rr(non)]] = CHIP_OFF; say("DRIFT: A PART DRIFTS OUT"); }
        request(s0);
    } else if (r < 48) { s0.prog ^= 1U; request(s0); say(s0.prog ? "DRIFT: OVER TO CHORDS B" : "DRIFT: OVER TO CHORDS A"); }
    else if (r < (nb ? 86 : 100)) mutate_part((unsigned)(non ? on[rr(non)] : rr(CHIP_PARTS)), wild);
    else { chip_bank_play((unsigned)banks[rr(nb)]); say("DRIFT: OVER TO A BANK"); }
}
/* ------------------------------------------------------------ recording notes from the keys into the loop */
struct held_rec { uint8_t on, part, step, which; int32_t start; };
static struct held_rec rec_key[32];
void chip_record_toggle(void)
{
    chip_recording ^= 1U;
    if (chip_recording && !chip_rt.playing) chip_play(1);
    say(chip_recording ? "RECORDING: PLAY THE KEYS INTO THE LOOP" : "RECORDING OFF");
}
static int note_to_degree(unsigned part, int note, int cd, int key)
{
    int base = (part == CHIP_BASS ? 24 : part == CHIP_CHORDS ? 48 : 60) + key, best = 0, bestd = 1000;
    const uint8_t *sc = chip_scale[chip_song.mood % CHIP_MOODS];
    int n = sc[0];
    for (int d = -14; d <= 30; ++d) {
        int dd = d, o = 0;
        while (dd < 0) { dd += n; --o; }
        while (dd >= n) { dd -= n; ++o; }
        int m = base + 12 * o + sc[1 + dd], diff = m - note;
        while (diff >= 12 && part != CHIP_LEAD) diff -= 12;  /* playback folds bass, arp and chords into range */
        int a = diff < 0 ? -diff : diff;
        if (a < bestd) { bestd = a; best = d - cd; }
    }
    return best;
}
void chip_record_down(unsigned part, unsigned key, unsigned note)
{
    if (!chip_recording || !chip_rt.playing || key >= 32 || part >= CHIP_PARTS) return;
    int32_t gs = chip_rt.gstep - 1;
    if (gs < 0) gs = 0;
    unsigned st = (unsigned)gs & 15U, bar = (unsigned)(gs >> 4);
    struct chip_state p = pending();
    if (!p.lv[part]) { p.lv[part] = CHIP_FULL; request(p); }
    unsigned which = p.lv[part] == CHIP_LIGHT ? 0 : 1;
    if (part == CHIP_DRUMS) {
        static const uint8_t drum[12] = {0, 1, 2, 3, 4, 5, 5, 5, 8, 3, 4, 1};
        chip_song.kit.drums[which][st] |= (uint16_t)(1U << drum[note % 12]);
    } else {
        int cd = chip_song.progs[p.prog & 1][bar & 3];
        struct chip_note *c = &chip_song.kit.mel[part][which][st];
        c->o = (int8_t)note_to_degree(part, (int)note, cd, chip_song.key + p.lift);
        c->d = 1;
        rec_key[key].on = 1; rec_key[key].part = (uint8_t)part; rec_key[key].step = (uint8_t)st; rec_key[key].which = (uint8_t)which; rec_key[key].start = gs;
    }
    ++chip_revision;
}
void chip_record_up(unsigned key)
{
    if (key >= 32 || !rec_key[key].on) return;
    rec_key[key].on = 0;
    int32_t len = chip_rt.gstep - rec_key[key].start;
    int max = rec_key[key].part == CHIP_LEAD || rec_key[key].part == CHIP_ARP ? 8 : 16;
    chip_song.kit.mel[rec_key[key].part][rec_key[key].which][rec_key[key].step].d = (uint8_t)clampi(len, 1, max);
    ++chip_revision;
}

void chip_cycle_part(unsigned part)
{
    if (part >= CHIP_PARTS) return;
    struct chip_state n = pending();
    n.lv[part] = (uint8_t)((n.lv[part] + 1) % 3);
    request(n);
}
void chip_toggle_prog(void) { struct chip_state n = pending(); n.prog ^= 1U; request(n); say(n.prog ? "PROGRESSION B" : "PROGRESSION A"); }
void chip_toggle_lift(void) { struct chip_state n = pending(); n.lift = (int8_t)(n.lift ? 0 : 2); request(n); say(n.lift ? "KEY LIFT +2" : "KEY LIFT OFF"); }
void chip_fill(void) { chip_rt.fill_req = 1; say("FILL"); }
void chip_bank_play(unsigned bank)
{
    if (bank >= CHIP_BANKS) return;
    if (!chip_banks[bank].used) { say("EMPTY BANK: SAVE + KEY SAVES IT"); return; }
    struct chip_state n = chip_banks[bank].st; n.bank = (int8_t)bank;
    request(n);
}
void chip_bank_save(unsigned bank)
{
    if (bank >= CHIP_BANKS) return;
    chip_banks[bank].kit = chip_song.kit;
    chip_banks[bank].st = pending(); chip_banks[bank].st.bank = (int8_t)bank;
    chip_banks[bank].used = 1;
    if (!chip_rt.has_target) chip_rt.live.bank = (int8_t)bank; else chip_rt.target.bank = (int8_t)bank;
    static const char *const saved[CHIP_BANKS] = {"SAVED TO BANK 1", "SAVED TO BANK 2", "SAVED TO BANK 3", "SAVED TO BANK 4",
                                                  "SAVED TO BANK 5", "SAVED TO BANK 6", "SAVED TO BANK 7", "SAVED TO BANK 8"};
    say(saved[bank]);
}
void chip_bank_delete(unsigned bank)
{
    if (bank >= CHIP_BANKS || !chip_banks[bank].used) return;
    chip_banks[bank].used = 0;
    if (chip_rt.live.bank == (int8_t)bank) chip_rt.live.bank = -1;
    say("BANK DELETED");
}
void chip_song_mode(void)
{
    int any = 0;
    for (int i = 0; i < CHIP_BANKS; ++i) any |= chip_banks[i].used;
    if (!any) { say("SAVE BANKS FIRST: SAVE + WHITE 1-8"); return; }
    chip_rt.song_mode ^= 1U;
    chip_rt.song_bar = 0;
    if (chip_rt.song_mode && !chip_rt.playing) {
        for (int i = 0; i < CHIP_BANKS; ++i) if (chip_banks[i].used) { chip_bank_play((unsigned)i); break; }
        chip_play(1);
    }
    say(chip_rt.song_mode ? "SONG: BANKS IN ORDER" : "SONG OFF");
}
void chip_new_patterns(unsigned part) { gen_part(part); ++chip_revision; say("NEW PATTERN"); }
void chip_new_progressions(void) { new_progs(); ++chip_revision; say("NEW CHORDS"); }
static uint8_t remix_open;
static struct chip_state undo_state;
void chip_remix(void)
{
    if (remix_open && now_ms - remix_at < 15000U) { remix_open = 0; (void)chip_undo(); say("BACK TO THE LAST SONG"); return; }
    undo.valid = 1; undo.song = chip_song; memcpy(undo.banks, chip_banks, sizeof chip_banks); undo_state = chip_rt.live;
    unsigned echo = chip_song.echo;
    new_project(chip_song.mood);
    chip_song.echo = (uint8_t)echo; chip_dsp_mix();
    memset(chip_banks, 0, sizeof chip_banks);
    chip_rt.song_mode = 0;
    struct chip_state s0; memset(&s0, 0, sizeof s0); s0.bank = -1;
    s0.lv[CHIP_ARP] = CHIP_FULL; s0.lv[CHIP_BASS] = CHIP_FULL; s0.lv[CHIP_DRUMS] = CHIP_FULL;
    chip_rt.live.bank = -1; request(s0);
    remix_open = 1; remix_at = now_ms;
    ++chip_revision;
    say("REMIX: ENV AGAIN GOES BACK");
}
void chip_new_loop(void)
{
    undo.valid = 1; undo.song = chip_song; memcpy(undo.banks, chip_banks, sizeof chip_banks); undo_state = chip_rt.live;
    unsigned echo = chip_song.echo;
    new_project(chip_song.mood);
    chip_song.echo = (uint8_t)echo; chip_dsp_mix();
    struct chip_state s0; memset(&s0, 0, sizeof s0); s0.bank = -1; s0.lv[CHIP_ARP] = CHIP_FULL;
    request(s0);
    say("A NEW LOOP: BRING PARTS IN");
}
int chip_undo(void)
{
    if (!undo.valid) { say("NOTHING TO UNDO"); return -1; }
    static struct chip_song s; static struct chip_bank b[CHIP_BANKS];   /* static: the control task's stack stays small */
    s = chip_song; memcpy(b, chip_banks, sizeof b);
    chip_song = undo.song; memcpy(chip_banks, undo.banks, sizeof chip_banks);
    undo.song = s; memcpy(undo.banks, b, sizeof b);
    chip_dsp_tempo(chip_song.bpm); chip_dsp_mix(); chip_refresh_sounds();
    { struct chip_state t = chip_rt.has_target ? chip_rt.target : chip_rt.live; request(undo_state); undo_state = t; }
    ++chip_revision;
    say("UNDONE: LFO REDOES");
    return 0;
}
void chip_mood(int delta)
{
    int base = chip_rt.mood_next >= 0 ? chip_rt.mood_next : chip_song.mood;
    int m = ((base + delta) % CHIP_MOODS + CHIP_MOODS) % CHIP_MOODS;
    if (chip_rt.playing) chip_rt.mood_next = (int8_t)m; else chip_song.mood = (uint8_t)m;
    ++chip_revision;
}
void chip_key(int delta)
{
    int base = chip_rt.key_next >= 0 ? chip_rt.key_next : chip_song.key;
    int k = ((base + delta) % 12 + 12) % 12;
    if (chip_rt.playing) chip_rt.key_next = (int8_t)k; else chip_song.key = (uint8_t)k;
    ++chip_revision;
}
void chip_tempo(int delta) { chip_song.bpm = (uint8_t)clampi(chip_song.bpm + delta, 40, 240); chip_dsp_tempo(chip_song.bpm); ++chip_revision; }
void chip_patch(unsigned part, int delta)
{
    if (part >= CHIP_PARTS) return;
    chip_song.patch[part] = (uint8_t)(((chip_song.patch[part] + delta) % 3 + 3) % 3);
    chip_refresh_sounds();
    if (!chip_rt.playing) chip_live_note(part, part == CHIP_DRUMS ? 0 : 4);
    ++chip_revision;
}
void chip_level(unsigned part, int delta)
{
    if (part >= CHIP_PARTS) return;
    chip_song.level[part] = (uint8_t)clampi(chip_song.level[part] + delta * 5, 0, 100);
    chip_dsp_mix(); ++chip_revision;
}
void chip_mute(unsigned part)
{
    if (part >= CHIP_PARTS) return;
    chip_song.mute[part] ^= 1U; chip_dsp_mix(); ++chip_revision;
}
void chip_arp_style(int delta)
{
    int style = chip_song.arp_dir * 3 + chip_song.arp_rate;
    style = ((style + delta) % 12 + 12) % 12;
    chip_song.arp_dir = (uint8_t)(style / 3); chip_song.arp_rate = (uint8_t)(style % 3);
    if (chip_song.arp_dir == 3) for (int i = 0; i < 8; ++i) arp_random[i] = (uint8_t)rr(16);
    build_arp(); ++chip_revision;
}
void chip_echo(int delta) { chip_song.echo = (uint8_t)clampi(chip_song.echo + delta, 0, 10); chip_dsp_mix(); ++chip_revision; }

const char *chip_part_name(unsigned part) { return part < CHIP_PARTS ? part_names[part] : ""; }
const char *chip_patch_name(unsigned part) { return part < CHIP_PARTS ? patch_names[part][chip_song.patch[part] % 3] : ""; }
const char *chip_mood_name(void) { return mood_names[(chip_rt.mood_next >= 0 ? chip_rt.mood_next : chip_song.mood) % CHIP_MOODS]; }
const char *chip_key_name(void) { return key_names[(chip_rt.key_next >= 0 ? chip_rt.key_next : chip_song.key) % 12]; }
const char *chip_arp_name(void)
{
    static char name[20];
    strcpy(name, arp_dirs[chip_song.arp_dir & 3]); strcat(name, " "); strcat(name, arp_rates[chip_song.arp_rate % 3]);
    return name;
}

/* ------------------------------------------------------------ the pages' edits */
void chip_env_edit(unsigned part, unsigned param, int delta)
{
    if (part >= 4 || param >= 4) return;
    chip_song.env[part][param] = (uint8_t)clampi(chip_song.env[part][param] + delta * 2, 0, 100);
    chip_refresh_sounds(); ++chip_revision;
}
void chip_vib_edit(unsigned part, unsigned param, int delta)
{
    if (part >= 4 || param >= 3) return;
    chip_song.vib[part][param] = (uint8_t)clampi(chip_song.vib[part][param] + delta * 2, 0, 100);
    chip_refresh_sounds(); ++chip_revision;
}
void chip_echo_time(int delta) { chip_song.echo_time = (uint8_t)clampi(chip_song.echo_time + delta, 0, 2); chip_dsp_tempo(chip_song.bpm); ++chip_revision; }
void chip_echo_feedback(int delta) { chip_song.echo_fb = (uint8_t)clampi(chip_song.echo_fb + delta, 0, 10); chip_dsp_mix(); ++chip_revision; }
void chip_arp_param(unsigned param, int delta)
{
    struct chip_song *s = &chip_song;
    if (param == 0) { s->arp_dir = (uint8_t)(((s->arp_dir + delta) % 4 + 4) % 4); if (s->arp_dir == 3) for (int i = 0; i < 8; ++i) arp_random[i] = (uint8_t)rr(16); }
    else if (param == 1) s->arp_rate = (uint8_t)clampi(s->arp_rate + delta, 0, 2);
    else if (param == 2) s->arp_range = (uint8_t)clampi(s->arp_range + delta, 1, 3);
    else s->arp_len = (uint8_t)clampi(s->arp_len + delta, 0, 2);
    if (param < 3) build_arp();
    ++chip_revision;
}
void chip_prog_chord(unsigned prog, unsigned slot, int delta)
{
    if (prog > 1 || slot > 3) return;
    chip_song.progs[prog][slot] = (uint8_t)(((chip_song.progs[prog][slot] + delta) % 7 + 7) % 7);
    ++chip_revision;
}
/* Step editing: the part's light (0) or full (1) pattern. */
void chip_step_toggle(unsigned part, unsigned which, unsigned step, int degree, unsigned drum)
{
    if (part >= CHIP_PARTS || which > 1 || step >= CHIP_STEPS) return;
    if (part == CHIP_DRUMS) chip_song.kit.drums[which][step] ^= (uint16_t)(1U << (drum > 8 ? 0 : drum));
    else {
        struct chip_note *n = &chip_song.kit.mel[part][which][step];
        if (n->d) n->d = 0; else { n->o = (int8_t)clampi(degree, -7, 21); n->d = (uint8_t)(part == CHIP_ARP ? 1 : 2); }
    }
    ++chip_revision;
}
void chip_step_pitch(unsigned part, unsigned which, unsigned step, int delta)
{
    if (part >= 4 || which > 1 || step >= CHIP_STEPS) return;
    struct chip_note *n = &chip_song.kit.mel[part][which][step];
    if (!n->d) { n->d = (uint8_t)(part == CHIP_ARP ? 1 : 2); n->o = 0; }
    else n->o = (int8_t)clampi(n->o + delta, -7, 21);
    ++chip_revision;
}
void chip_step_length(unsigned part, unsigned which, unsigned step, int delta)
{
    if (part >= 4 || which > 1 || step >= CHIP_STEPS) return;
    struct chip_note *n = &chip_song.kit.mel[part][which][step];
    if (!n->d) return;
    n->d = (uint8_t)clampi(n->d + delta, 1, 16);
    ++chip_revision;
}
void chip_pattern_copy(unsigned part, unsigned to)
{
    if (part >= CHIP_PARTS || to > 1) return;
    if (part == CHIP_DRUMS) memcpy(chip_song.kit.drums[to], chip_song.kit.drums[to ^ 1], sizeof chip_song.kit.drums[0]);
    else memcpy(chip_song.kit.mel[part][to], chip_song.kit.mel[part][to ^ 1], sizeof chip_song.kit.mel[part][0]);
    ++chip_revision;
    say(to ? "LIGHT COPIED TO FULL" : "FULL COPIED TO LIGHT");
}

/* ------------------------------------------------------------ songs kept in Flash: eight slots, saved by themselves */
enum { STORE_MAGIC = 0x53314942U /* 'BI1S' */, STORE_VERSION = 1, STORE_SLOTS = 8 };
struct record { uint32_t magic, version, generation, crc; struct chip_song song; struct chip_bank banks[CHIP_BANKS]; struct chip_state live; };
_Static_assert(sizeof(struct record) <= FM1_FLASH_SECTOR_BYTES, "a song fits one Flash sector");
static struct record rec;
static char slot_names[STORE_SLOTS][20];
static uint8_t slot_known[STORE_SLOTS];
static uint32_t generation, saved_crc, seen_crc, changed_at, last_check;
uint8_t chip_slot;
static uint32_t crc32(const void *data, unsigned n, uint32_t c)
{
    const uint8_t *p = data; c = ~c;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U))); }
    return ~c;
}
static uint32_t state_crc(void)
{
    uint32_t c = crc32(&chip_song, sizeof chip_song, 0);
    c = crc32(chip_banks, sizeof chip_banks, c);
    return crc32(chip_rt.live.lv, sizeof chip_rt.live.lv, c ^ chip_rt.live.prog);
}
static int slot_read(unsigned slot)
{
    if (slot >= STORE_SLOTS || fm1_flash_size() < STORE_SLOTS * FM1_FLASH_SECTOR_BYTES) return -1;
    if (fm1_flash_read(slot * FM1_FLASH_SECTOR_BYTES, &rec, sizeof rec)) return -1;
    if (rec.magic != STORE_MAGIC || rec.version != STORE_VERSION) return -1;
    return rec.crc == crc32(&rec.song, (unsigned)(sizeof rec - offsetof(struct record, song)), 0) ? 0 : -1;
}
int chip_store_save(unsigned slot)
{
    if (slot >= STORE_SLOTS || fm1_flash_size() < STORE_SLOTS * FM1_FLASH_SECTOR_BYTES) return -1;
    memset(&rec, 0, sizeof rec);
    rec.magic = STORE_MAGIC; rec.version = STORE_VERSION; rec.generation = ++generation;
    rec.song = chip_song; memcpy(rec.banks, chip_banks, sizeof chip_banks);
    rec.live = chip_rt.has_target ? chip_rt.target : chip_rt.live;
    rec.crc = crc32(&rec.song, (unsigned)(sizeof rec - offsetof(struct record, song)), 0);
    uint32_t at = slot * FM1_FLASH_SECTOR_BYTES;
    if (fm1_flash_erase(at) || fm1_flash_write(at, &rec, sizeof rec)) return -1;
    slot_known[chip_slot] = 0; chip_slot = (uint8_t)slot; slot_known[slot] = 0;
    saved_crc = seen_crc = state_crc();
    return 0;
}
int chip_store_load(unsigned slot)
{
    if (slot_read(slot)) return -1;
    undo.valid = 1; undo.song = chip_song; memcpy(undo.banks, chip_banks, sizeof chip_banks); undo_state = chip_rt.live;
    chip_song = rec.song; memcpy(chip_banks, rec.banks, sizeof chip_banks);
    chip_rt.song_mode = 0;
    request(rec.live);
    chip_dsp_tempo(chip_song.bpm); chip_dsp_mix(); chip_refresh_sounds(); build_arp();
    slot_known[chip_slot] = 0; chip_slot = (uint8_t)slot;
    saved_crc = seen_crc = state_crc();
    ++chip_revision;
    return 0;
}
const char *chip_store_name(unsigned slot)
{
    if (slot >= STORE_SLOTS) return "";
    if (slot == chip_slot) return chip_song.name;
    if (!slot_known[slot]) {
        slot_known[slot] = 1;
        if (!slot_read(slot)) memcpy(slot_names[slot], rec.song.name, sizeof slot_names[slot]);
        else strcpy(slot_names[slot], "EMPTY");
        slot_names[slot][19] = 0;
    }
    return slot_names[slot];
}
static void store_boot(void)
{
    int best = -1; uint32_t best_gen = 0;
    for (unsigned i = 0; i < STORE_SLOTS; ++i)
        if (!slot_read(i)) { if (best < 0 || (int32_t)(rec.generation - best_gen) > 0) { best = (int)i; best_gen = rec.generation; } if (rec.generation > generation) generation = rec.generation; }
    if (best >= 0 && !slot_read((unsigned)best)) {
        chip_song = rec.song; memcpy(chip_banks, rec.banks, sizeof chip_banks); chip_rt.live = rec.live;
        chip_slot = (uint8_t)best;
        chip_dsp_tempo(chip_song.bpm); chip_dsp_mix(); chip_refresh_sounds(); build_arp();
        saved_crc = seen_crc = state_crc();
        say("WELCOME BACK");
    }
}
/* Autosave: once the song has stayed unchanged for six seconds, it is written to its slot. */
static void store_poll(uint32_t now)
{
    if (now - last_check < 1000U) return;
    last_check = now;
    uint32_t c = state_crc();
    if (c != seen_crc) { seen_crc = c; changed_at = now; return; }
    if (c != saved_crc && now - changed_at >= 6000U) (void)chip_store_save(chip_slot);
}
