/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * bi-1 chip synth: the sound chip and the bar-by-bar sequencer.
 * Runs inside the audio interrupt. Integer arithmetic only, no library calls,
 * no division, no 64-bit maths, code and tables in RAM, so it keeps running
 * while the FM-1 writes Flash. Tables are filled by chip_dsp_init() from the
 * control task before playback starts. */
#include "chip.h"
#include "chip_dsp.h"

#define AC FM1_AUDIO_CODE
#define AD FM1_AUDIO_DATA

enum { FS = 44100, TICK = 735 }; /* 60 Hz frame ticks, like the consoles */
enum { VOICES = 20, ECHO_LEN = 16384 };
enum { W_NONE, W_P12, W_P25, W_P50, W_TRI, W_WAVE, W_SAW, W_NOISE_L, W_NOISE_S };

/* ------------------------------------------------------------ tables (filled by chip_dsp_init) */
uint32_t chip_inc[130] AD;            /* phase increment per MIDI note */
static uint32_t recip[257] AD;         /* 65536 / n, so the interrupt never divides */
int32_t chip_cut[64] AD;              /* SVF coefficient, 100 Hz .. 9 kHz, Q15 */
int16_t chip_wave32[32] AD, chip_tri32[32] AD;
struct chip_patch chip_patches[CHIP_PATCHES] AD;
struct chip_patch chip_part_patch[CHIP_PARTS] AD;   /* each part's sound with its ENV and LFO edits */
uint8_t chip_part_patch_id[CHIP_PARTS] AD = {255, 255, 255, 255, 255};
int32_t chip_echo_fb AD = 9830, chip_echo_ret AD = 11469;
struct chip_drum_consts chip_dc AD;
uint32_t chip_step_samples AD = 5000; /* samples per 16th, from the control task */
uint32_t chip_ticks_per_step_q8 AD = 1700;
uint32_t chip_echo_samples AD = 8000;
int32_t chip_part_gain[CHIP_PARTS] AD; /* Q15, level^2 x 1.3 x .85, 0 when muted */
int32_t chip_echo_send[CHIP_PARTS] AD; /* Q15 */
uint8_t chip_scale[CHIP_MOODS][8] AD;  /* count then steps */

/* ------------------------------------------------------------ voices */
struct voice {
    uint8_t on, part, w1, w2, filt, kind, chord_n, chord_i, key; /* key: panel key + 1 while held */
    uint8_t chord[4];
    int16_t mix1, mix2;            /* Q15 */
    int32_t note, note_end;        /* note x 256 */
    int16_t drop_ticks, drop_age;
    int16_t vib_depth, vib_delay;  /* note x 256, ticks */
    uint16_t vib_phase, vib_inc;
    int32_t ratio2;                /* osc2 frequency, Q16 of osc1 */
    uint32_t ph1, ph2, inc1, inc2;
    uint32_t nclk, ninc; uint16_t lfsr; uint8_t nshort; int16_t nval;
    int32_t env, sus, kd, kr;      /* Q16 */
    int16_t atk, age, gate;        /* ticks */
    uint8_t stage;                 /* 0 attack, 1 hold/decay, 2 release */
    int32_t amp;                   /* Q15 current output level */
    int32_t pk;                    /* Q15 peak */
    int32_t cut, cut_target, cut_k; /* index x 256, Q16 */
    int32_t q, lp, bp;
    /* noise sweep (the riser) */
    int32_t sweep_from, sweep_to; int16_t sweep_ticks;
};
static struct voice v[VOICES];
static int16_t echo_buf[ECHO_LEN];
static uint32_t echo_w;
static int32_t echo_lp, hp_x, hp_y;
static int32_t tick_left, step_left;
static int32_t duck[CHIP_PARTS], duck_target[CHIP_PARTS], duck_step[CHIP_PARTS]; /* Q16 */
struct tail { int32_t until; uint8_t lv, prog; int8_t lift; uint8_t on; };
static struct tail tails[CHIP_PARTS];
static int32_t fill_at = -1;
static volatile uint8_t live_req_part[16], live_req_deg[16], live_req_kind[16], live_req_key[16]; /* kind 0 preview, 1 key down, 2 key up */
static volatile uint32_t live_w, live_r;


static AC uint32_t note_inc(int32_t note_q8)
{
    if (note_q8 < 0) note_q8 = 0;
    if (note_q8 > 128 * 256) note_q8 = 128 * 256;
    int32_t n = note_q8 >> 8, f = note_q8 & 255;
    uint32_t a = chip_inc[n], b = chip_inc[n + 1];
    return a + ((b - a) >> 8) * (uint32_t)f;
}
static AC int16_t wave(unsigned w, uint32_t ph)
{
    switch (w) {
    case W_P12: return ph < 0x20000000u ? 24000 : -24000;
    case W_P25: return ph < 0x40000000u ? 24000 : -24000;
    case W_P50: return ph < 0x80000000u ? 24000 : -24000;
    case W_TRI: return chip_tri32[ph >> 27];
    case W_WAVE: return chip_wave32[ph >> 27];
    case W_SAW: return (int16_t)((int32_t)(ph >> 17) - 16384) * 3 / 2;
    default: return 0;
    }
}
static AC struct voice *grab(void)
{
    struct voice *best = &v[0];
    for (unsigned i = 0; i < VOICES; ++i) {
        if (!v[i].on) return &v[i];
        if (v[i].stage > best->stage || (v[i].stage == best->stage && v[i].amp < best->amp)) best = &v[i];
    }
    return best;
}
static AC void clear_voice(struct voice *x)
{
    uint8_t *p = (uint8_t *)x;
    for (unsigned i = 0; i < sizeof *x; ++i) p[i] = 0;
}

/* ------------------------------------------------------------ starting notes */
/* gate in ticks; vel Q8 (256 = 1) */
static AC struct voice *start_patch(unsigned part, unsigned patch, int note, int gate, int vel_q8, const uint8_t *chord, unsigned chord_n)
{
    const struct chip_patch *p = chip_part_patch_id[part] == patch ? &chip_part_patch[part] : &chip_patches[patch];
    struct voice *x = grab();
    clear_voice(x);
    x->on = 1; x->part = (uint8_t)part; x->kind = 0;
    x->w1 = p->w1; x->w2 = p->w2; x->mix1 = p->mix1; x->mix2 = p->mix2; x->ratio2 = p->ratio2;
    x->note = note * 256; x->note_end = x->note;
    x->vib_depth = p->vib_depth; x->vib_delay = p->vib_delay; x->vib_inc = p->vib_inc;
    x->atk = p->atk; x->kd = p->kd; x->kr = p->kr; x->sus = p->sus;
    x->pk = (p->pk * vel_q8) >> 8;
    x->gate = (int16_t)(gate < 1 ? 1 : gate > 900 ? 900 : gate);
    x->filt = p->filt;
    if (p->filt) { x->cut = p->cut_start; x->cut_target = p->cut_end; x->cut_k = p->cut_k; x->q = p->q; }
    if (chord_n) { for (unsigned i = 0; i < chord_n && i < 4; ++i) x->chord[i] = chord[i]; x->chord_n = (uint8_t)chord_n; x->note = chord[0] * 256; }
    x->inc1 = note_inc(x->note);
    x->inc2 = (uint32_t)(((uint64_t)x->inc1 * (uint32_t)x->ratio2) >> 16);
    x->env = x->atk ? 0 : 65536;
    x->stage = x->atk ? 0 : 1;
    x->amp = x->atk ? 0 : x->pk;
    chip_rt.note_on[part] = 8;
    return x;
}
/* A drum: a falling tone and/or a noise burst, with the app's 4-bit decay. */
static AC void start_drum_voice(unsigned w, int32_t n0, int32_t n1, int frames, uint32_t ninc, uint8_t nshort, int32_t pk, int32_t kd, int gate)
{
    struct voice *x = grab();
    clear_voice(x);
    x->on = 1; x->part = CHIP_DRUMS; x->kind = 1;
    x->w1 = (uint8_t)w; x->mix1 = 32767;
    x->note = n0; x->note_end = n1; x->drop_ticks = (int16_t)frames;
    x->inc1 = note_inc(n0);
    x->ninc = ninc; x->nshort = nshort; x->lfsr = 1;
    x->pk = pk; x->amp = pk; x->env = 65536; x->stage = 1; x->sus = 0; x->kd = kd; x->kr = chip_dc.k_release;
    x->gate = (int16_t)gate;
}
static AC uint32_t noise_rate(uint32_t base, int32_t rate_q8) { return (uint32_t)(((uint64_t)base * (uint32_t)rate_q8) >> 8); }
static AC void drum_hit(unsigned pc, int vel_q8)
{
    unsigned kit = chip_song.patch[CHIP_DRUMS] % 3;
    const struct chip_drum_consts *d = &chip_dc;
    unsigned tone = kit == 0 ? W_TRI : kit == 1 ? W_P50 : W_SAW;
    int32_t nr = d->noise_rate[kit];   /* Q8 */
#define PK(x) (((int32_t)(x) * vel_q8) >> 8)
    switch (pc) {
    case 0: start_drum_voice(tone, d->kick_from, d->kick_to, 6, 0, 0, PK(kit == 1 ? d->lvl_kick_gb : d->lvl_kick), d->k_kick, d->g_kick); break;
    case 1: start_drum_voice(W_NOISE_L, 0, 0, 0, noise_rate(d->noise_inc, nr * 230 >> 8), 0, PK(d->lvl_snare), d->k_snare, d->g_snare);
            start_drum_voice(W_P50, d->snare_from, d->snare_to, 3, 0, 0, PK(d->lvl_snare_tone), d->k_snare_tone, d->g_snare_tone); break;
    case 2: start_drum_voice(W_NOISE_L, 0, 0, 0, noise_rate(d->noise_inc, nr * 154 >> 8), 0, PK(d->lvl_clap), d->k_clap, d->g_clap); break;
    case 3: start_drum_voice(W_NOISE_S, 0, 0, 0, noise_rate(d->noise_short_inc, nr), 1, PK(d->lvl_hat), d->k_hat, d->g_hat); break;
    case 4: start_drum_voice(W_NOISE_L, 0, 0, 0, noise_rate(d->noise_inc, nr * 333 >> 8), 0, PK(d->lvl_open), d->k_open, d->g_open); break;
    case 5: case 6: case 7: {
        unsigned t = tone == W_SAW ? W_P50 : tone, i = pc - 5;
        start_drum_voice(t, d->tom_from[i], d->tom_to[i], 10, 0, 0, PK(kit == 1 ? d->lvl_tom_gb : d->lvl_tom), d->k_tom, d->g_tom); break; }
    default: start_drum_voice(W_NOISE_L, 0, 0, 0, noise_rate(d->noise_inc, nr * 205 >> 8), 0, PK(d->lvl_crash), d->k_crash, d->g_crash); break;
    }
#undef PK
    chip_rt.note_on[CHIP_DRUMS] = 8;
}
static AC void riser(int steps)
{
    struct voice *x = grab();
    clear_voice(x);
    x->on = 1; x->part = CHIP_DRUMS; x->kind = 2; x->w1 = W_NOISE_L; x->mix1 = 32767; x->lfsr = 1;
    x->sweep_ticks = (int16_t)((steps * (int32_t)chip_ticks_per_step_q8) >> 8);
    if (x->sweep_ticks > 255) x->sweep_ticks = 255;
    if (x->sweep_ticks < 1) x->sweep_ticks = 1;
    x->sweep_from = (int32_t)(noise_rate(chip_dc.noise_inc, 38) >> 8);   /* rate .15, stored >> 8 */
    x->sweep_to = (int32_t)(noise_rate(chip_dc.noise_inc, 500) >> 8);    /* rate 1.95 */
    x->ninc = (uint32_t)x->sweep_from << 8;
    x->pk = chip_dc.lvl_riser; x->stage = 1; x->env = 0;
}

/* ------------------------------------------------------------ notes from patterns */
static AC int midi_of(int base, int degree)
{
    const uint8_t *s = chip_scale[chip_song.mood % CHIP_MOODS];
    int n = s[0], o = 0;
    while (degree < 0) { degree += n; --o; }
    while (degree >= n) { degree -= n; ++o; }
    return base + 12 * o + s[1 + degree];
}
static AC int gate_ticks(int steps, int factor_q8)
{
    return (int)((((uint32_t)steps * chip_ticks_per_step_q8) >> 4) * (uint32_t)factor_q8 >> 12);
}
static const uint16_t arp_len_q8[3] AD = {90, 205, 346}; /* .35 .8 1.35 */
static AC void part_events(unsigned part, unsigned lv, int32_t gs, unsigned prog, int lift, int skip_drums_but_kick)
{
    if (!lv || chip_song.mute[part]) return;
    unsigned st = (unsigned)gs & 15U, bar = (unsigned)(gs >> 4);
    int cd = chip_song.progs[prog & 1][bar & 3], key = chip_song.key + lift;
    const struct chip_kit *k = &chip_song.kit;
    unsigned which = lv == CHIP_LIGHT ? 0 : 1;
    if (part == CHIP_DRUMS) {
        uint16_t m = k->drums[which][st];
        for (unsigned pc = 0; pc < 9; ++pc)
            if ((m >> pc) & 1U) {
                if (skip_drums_but_kick && pc) continue;
                drum_hit(pc, pc == 3 ? 128 : pc == 4 ? 154 : 230);
            }
        return;
    }
    const struct chip_note *n = &k->mel[part][which][st];
    if (!n->d) return;
    int sd_factor = part == CHIP_LEAD ? 243 : part == CHIP_ARP ? arp_len_q8[chip_song.arp_len % 3] : part == CHIP_CHORDS ? 251 : 218;
    int gate = gate_ticks(n->d, sd_factor);
    if (part == CHIP_CHORDS) {
        uint8_t notes[4]; unsigned cnt = 0;
        static const int8_t add[4] AD = {0, 2, 4, 6};
        for (unsigned i = 0; i < 4; ++i) {
            int m = midi_of(48 + key, cd + n->o + add[i]);
            while (m < 52) m += 12;
            while (m >= 70) m -= 12;
            notes[cnt++] = (uint8_t)m;
        }
        for (unsigned a = 0; a < cnt; ++a) for (unsigned b = a + 1; b < cnt; ++b) if (notes[b] < notes[a]) { uint8_t t = notes[a]; notes[a] = notes[b]; notes[b] = t; }
        unsigned patch = chip_song.patch[CHIP_CHORDS] % 3;
        int vel = lv == CHIP_LIGHT ? 179 : 218;
        if (patch == 0) start_patch(part, P_ARPCHORD, notes[0], gate, vel, notes, cnt);
        else for (unsigned i = 0; i < cnt; ++i) start_patch(part, patch == 1 ? P_PULSEPAD : P_WAVEPAD, notes[i], gate, vel, 0, 0);
        return;
    }
    int o = n->o;
    if (part == CHIP_LEAD && (bar & 3U) == 3U) {          /* the last note of the phrase comes home */
        int last = -1;
        for (unsigned i = 0; i < 16; ++i) if (k->mel[part][which][i].d) last = (int)i;
        if ((int)st == last) o = o >= 0 ? (o + 3) / 7 * 7 : -((-o + 3) / 7 * 7);
    }
    int m = midi_of((part == CHIP_BASS ? 24 : 60) + key, cd + o);
    if (part == CHIP_BASS) { while (m < 33) m += 12; while (m >= 45 + (o >= 7 ? 12 : 0)) m -= 12; }
    else { while (m > 88) m -= 12; while (m < 55) m += 12; }
    static const uint8_t base_patch[4][3] AD = {{P_PULSE50, P_PULSE25, P_SIDLEAD}, {P_PULSE12, P_PULSE25, P_WAVE}, {0, 0, 0}, {P_TRI, P_PULSEBASS, P_SIDBASS}};
    start_patch(part, base_patch[part][chip_song.patch[part] % 3], m, gate, lv == CHIP_LIGHT ? 205 : 256, 0, 0);
}

/* ------------------------------------------------------------ bar-line changes, as in the app */
static AC void duck_to(unsigned part, int32_t target, int32_t samples)
{
    duck_target[part] = target;
    int32_t diff = target - duck[part], blocks = samples >> 7;
    if (blocks < 1) { duck[part] = target; duck_step[part] = 0; return; }
    int32_t step = 0; /* diff / blocks without division */
    int32_t lo = 0, hi = 65536;
    int32_t ad = diff < 0 ? -diff : diff;
    while (lo < hi) { int32_t mid = (lo + hi + 1) >> 1; if (mid * blocks <= ad) lo = mid; else hi = mid - 1; }
    step = diff < 0 ? -lo : lo;
    duck_step[part] = step ? step : (diff < 0 ? -1 : 1);
}
static AC void copy_kit(struct chip_kit *to, const struct chip_kit *from)
{
    uint32_t *a = (uint32_t *)to; const uint32_t *b = (const uint32_t *)from;
    for (unsigned i = 0; i < sizeof *to / 4; ++i) a[i] = b[i];
}
static AC void apply_change(void)
{
    struct chip_state old = chip_rt.live, nw = chip_rt.target;
    int32_t bar = (int32_t)chip_step_samples * 16, crash = 0;
    chip_rt.has_target = 0; chip_rt.change_at = -1;
    for (unsigned l = 0; l < CHIP_PARTS; ++l) {
        unsigned a = old.lv[l], b = nw.lv[l];
        if (!a && b) {
            tails[l].on = 0;
            if (l == CHIP_CHORDS || l == CHIP_ARP) { duck[l] = 0; duck_to(l, 65536, bar >> 1); }
            else if (l == CHIP_LEAD) { duck[l] = 0; duck_to(l, 65536, (int32_t)chip_step_samples * 4); }
            else { duck[l] = 65536; duck_to(l, 65536, 0); }
            if (l == CHIP_DRUMS) crash = 1;
        } else if (a && !b) {
            if (l == CHIP_DRUMS) crash = 1;
            else if (l == CHIP_BASS) {
                unsigned barn = (unsigned)(chip_rt.gstep >> 4);
                int m = midi_of(24 + chip_song.key, chip_song.progs[old.prog & 1][barn & 3]);
                while (m < 33) m += 12;
                while (m >= 45) m -= 12;
                if (!chip_song.mute[l]) start_patch(l, (uint8_t[]){P_TRI, P_PULSEBASS, P_SIDBASS}[chip_song.patch[l] % 3], m, gate_ticks(12, 256), 230, 0, 0);
            } else {
                tails[l].on = 1; tails[l].lv = (uint8_t)a; tails[l].until = chip_rt.gstep + 16; tails[l].prog = old.prog; tails[l].lift = old.lift;
                duck_to(l, 0, bar);
            }
        } else if (a && b && a != b && l == CHIP_DRUMS) crash = b > a;
    }
    if (crash && !chip_song.mute[CHIP_DRUMS]) drum_hit(8, 128);
    chip_rt.live = nw;
    if (nw.bank >= 0 && nw.bank < CHIP_BANKS && chip_banks[nw.bank].used && old.bank != nw.bank)
        copy_kit(&chip_song.kit, &chip_banks[nw.bank].kit);
}
static AC void step_tick(void)
{
    int32_t gs = chip_rt.gstep;
    unsigned st = (unsigned)gs & 15U;
    if (chip_rt.song_mode && st == 12) {
        int order[CHIP_BANKS], n = 0;
        for (int i = 0; i < CHIP_BANKS; ++i) if (chip_banks[i].used) order[n++] = i;
        if (n) {
            int32_t idx = (chip_rt.song_bar >> 2);
            while (idx >= n) idx -= n;
            int b = order[idx];
            if (chip_rt.live.bank != b || chip_rt.has_target) {
                chip_rt.target = chip_banks[b].st; chip_rt.target.bank = (int8_t)b;
                chip_rt.has_target = 1; chip_rt.change_at = gs + 4;
            }
        }
    }
    if (chip_rt.fill_req) { chip_rt.fill_req = 0; fill_at = (gs + 16) & ~15; if (fill_at - gs < 4) fill_at += 16; }
    int32_t left = chip_rt.has_target ? chip_rt.change_at - gs : 99, fill_left = fill_at >= 0 ? fill_at - gs : 99;
    if (chip_rt.has_target && left == 8) {
        unsigned changing = 0;
        for (unsigned l = 0; l < CHIP_PARTS; ++l) changing += chip_rt.target.lv[l] != chip_rt.live.lv[l];
        if (changing >= 2) riser(8);
    }
    if (chip_rt.has_target && left <= 0) apply_change();
    if (fill_left <= 0) fill_at = -1;
    if (st == 0 && chip_rt.song_mode) chip_rt.song_bar = chip_rt.song_bar + 1;
    if (st == 0 && chip_rt.mood_next >= 0) { chip_song.mood = (uint8_t)chip_rt.mood_next; chip_rt.mood_next = -1; }
    if (st == 0 && chip_rt.key_next >= 0) { chip_song.key = (uint8_t)chip_rt.key_next; chip_rt.key_next = -1; }
    int drums_here = chip_rt.live.lv[CHIP_DRUMS] || (chip_rt.has_target && chip_rt.target.lv[CHIP_DRUMS]);
    int fill = (left > 0 && left <= 4 && drums_here) || (fill_left > 0 && fill_left <= 4);
    for (unsigned l = 0; l < CHIP_PARTS; ++l)
        part_events(l, chip_rt.live.lv[l], gs, chip_rt.live.prog, chip_rt.live.lift, l == CHIP_DRUMS && fill);
    for (unsigned l = 0; l < CHIP_PARTS; ++l) {
        if (!tails[l].on) continue;
        if (gs >= tails[l].until) { tails[l].on = 0; duck[l] = 65536; duck_target[l] = 65536; duck_step[l] = 0; continue; }
        if (!chip_rt.live.lv[l]) part_events(l, tails[l].lv, gs, tails[l].prog, tails[l].lift, 0);
    }
    if (fill && !chip_song.mute[CHIP_DRUMS]) {
        int32_t k = left < fill_left ? left : fill_left;
        static const uint8_t toms[4] AD = {7, 6, 5, 5};
        drum_hit(toms[(k - 1) & 3], 154 + (4 - k) * 20);
        if (k <= 2) drum_hit(1, 102 + (2 - k) * 51);
    }
    chip_rt.gstep = gs + 1;
}

/* ------------------------------------------------------------ 60 Hz tick: envelopes, pitch, filters */
static AC void voice_tick(struct voice *x)
{
    /* pitch: drum drops, chord racing, vibrato */
    int32_t note = x->note;
    if (x->drop_ticks && x->drop_age < x->drop_ticks) {
        ++x->drop_age;
        note = x->note + (int32_t)(((int64_t)(x->note_end - x->note) * x->drop_age * (int64_t)recip[x->drop_ticks & 255]) >> 16);
        if (x->drop_age == x->drop_ticks) { x->note = x->note_end; x->drop_ticks = 0; note = x->note; }
        else { x->inc1 = note_inc(note); }
    }
    if (x->chord_n) { x->chord_i = (uint8_t)((x->chord_i + 1) >= x->chord_n ? 0 : x->chord_i + 1); note = x->chord[x->chord_i] * 256; x->note = note; }
    if (x->vib_depth && x->age >= x->vib_delay) {
        x->vib_phase = (uint16_t)(x->vib_phase + x->vib_inc);
        int32_t tri = x->vib_phase < 32768 ? (int32_t)x->vib_phase * 2 - 32768 : 98303 - (int32_t)x->vib_phase * 2; /* -32768..32767 */
        note = x->note + ((tri * x->vib_depth) >> 15);
    }
    if (!x->drop_ticks || x->kind != 1) x->inc1 = note_inc(note);
    if (x->w2) x->inc2 = (uint32_t)(((uint64_t)x->inc1 * (uint32_t)x->ratio2) >> 16);
    /* SID filter sweep: an exponential glide toward its resting cutoff */
    if (x->filt) x->cut = x->cut_target + (int32_t)(((int64_t)(x->cut - x->cut_target) * x->cut_k) >> 16);
    /* the riser: noise climbing in pitch and loudness */
    if (x->kind == 2) {
        ++x->age;
        if (x->age >= x->sweep_ticks) { x->on = 0; return; }
        int32_t frac = (int32_t)(((uint32_t)x->age * recip[x->sweep_ticks > 255 ? 255 : x->sweep_ticks]) >> 8);
        if ((x->age & 1) == 0) x->ninc = (uint32_t)(x->sweep_from + (((x->sweep_to - x->sweep_from) * frac) >> 8)) << 8;
        int32_t lvl = (frac * 12) >> 8;
        x->amp = (x->pk * lvl * 2731) >> 15;
        return;
    }
    /* 4-bit volume envelope on ticks */
    ++x->age;
    if (x->stage == 0) {
        x->env = x->age >= x->atk ? 65536 : (int32_t)((uint32_t)x->age * recip[x->atk & 255]);
        if (x->age >= x->atk) x->stage = 1;
    } else if (x->stage == 1) {
        x->env = x->sus + (int32_t)(((int64_t)(x->env - x->sus) * x->kd) >> 16);
        if (x->age >= x->gate) x->stage = 2;
    } else {
        x->env = (int32_t)(((int64_t)x->env * x->kr) >> 16);
        if (x->env < 2184) { x->on = 0; return; }
    }
    int32_t level = (x->env * 15 + 32768) >> 16;              /* sixteen steps */
    x->amp = (x->pk * level * 2185) >> 15;                    /* pk x level / 15 */
}

/* ------------------------------------------------------------ the block */
static int32_t busbuf[CHIP_PARTS][FM1_BLOCK_FRAMES];
static const uint8_t live_patch[4][3] AD = {{P_PULSE50, P_PULSE25, P_SIDLEAD}, {P_PULSE12, P_PULSE25, P_WAVE}, {P_PULSEPAD, P_PULSEPAD, P_WAVEPAD}, {P_TRI, P_PULSEBASS, P_SIDBASS}};
static const uint8_t live_drum[12] AD = {0, 1, 2, 3, 4, 5, 6, 7, 8, 3, 4, 1}; /* the twelve keys of each octave */
/* Played from the panel: the pressed note is the root; the chord's third and fifth come from the mood's scale. */
static AC void live_key_down(unsigned part, int note, unsigned key)
{
    if (part == CHIP_DRUMS) { int pc = note % 12; drum_hit(live_drum[pc < 0 ? 0 : pc], 230); return; }
    unsigned patch = chip_song.patch[part] % 3;
    if (part == CHIP_CHORDS) {
        const uint8_t *sc = chip_scale[chip_song.mood % CHIP_MOODS];
        int r = note - (int)chip_song.key, o = 0;
        while (r < 0) { r += 12; --o; }
        while (r >= 12) { r -= 12; ++o; }
        int i = 0;
        for (int k = 0; k < sc[0]; ++k) if (sc[1 + k] <= r) i = k;
        int base = midi_of((int)chip_song.key + 12 * o, i);
        uint8_t notes[3] = {(uint8_t)note, (uint8_t)(note + midi_of((int)chip_song.key + 12 * o, i + 2) - base), (uint8_t)(note + midi_of((int)chip_song.key + 12 * o, i + 4) - base)};
        if (patch == 0) { struct voice *x = start_patch(part, P_ARPCHORD, note, 900, 218, notes, 3); x->key = (uint8_t)(key + 1); }
        else for (unsigned n = 0; n < 3; ++n) { struct voice *x = start_patch(part, patch == 1 ? P_PULSEPAD : P_WAVEPAD, notes[n], 900, 218, 0, 0); x->key = (uint8_t)(key + 1); }
        return;
    }
    struct voice *x = start_patch(part, live_patch[part][patch], note, 900, 230, 0, 0);
    x->key = (uint8_t)(key + 1);
}
static AC void live_key_up(unsigned key)
{
    for (unsigned i = 0; i < VOICES; ++i)
        if (v[i].on && v[i].key == key + 1) { v[i].key = 0; if (v[i].stage < 2) v[i].stage = 2; }
}
AC void chip_render(int32_t *output, unsigned frames)
{
    if (!output || frames > FM1_BLOCK_FRAMES) return;
    while (live_r != live_w) {                                  /* notes from the panel keys */
        unsigned i = live_r & 15U, part = live_req_part[i], deg = live_req_deg[i], kind = live_req_kind[i], key = live_req_key[i];
        live_r = live_r + 1;
        if (kind == 1) live_key_down(part, (int)deg, key);
        else if (kind == 2) live_key_up(key);
        else if (part == CHIP_DRUMS) drum_hit(live_drum[deg % 12], 230);
        else {
            int base = part == CHIP_BASS ? 36 : part == CHIP_CHORDS ? 48 : 60;
            int m = midi_of(base + chip_song.key, (int)deg);
            start_patch(part, live_patch[part][chip_song.patch[part] % 3], m, 18, 230, 0, 0);
        }
    }
    if (chip_rt.stop_req) {
        chip_rt.stop_req = 0;
        for (unsigned i = 0; i < VOICES; ++i) if (v[i].on && v[i].kind != 2) { v[i].stage = 2; } else v[i].on = 0;
        for (unsigned l = 0; l < CHIP_PARTS; ++l) { tails[l].on = 0; duck[l] = duck_target[l] = 65536; duck_step[l] = 0; }
        fill_at = -1;
    }
    if (chip_rt.playing) {
        step_left -= (int32_t)frames;
        while (step_left <= 0) { step_tick(); step_left += (int32_t)chip_step_samples; }
    }
    tick_left -= (int32_t)frames;
    if (tick_left <= 0) {
        tick_left += TICK;
        for (unsigned i = 0; i < VOICES; ++i) if (v[i].on) voice_tick(&v[i]);
        for (unsigned l = 0; l < CHIP_PARTS; ++l) if (chip_rt.note_on[l]) chip_rt.note_on[l] = (uint8_t)(chip_rt.note_on[l] - 1);
    }
    for (unsigned l = 0; l < CHIP_PARTS; ++l) {
        if (duck[l] != duck_target[l]) {
            duck[l] += duck_step[l];
            if ((duck_step[l] > 0 && duck[l] > duck_target[l]) || (duck_step[l] < 0 && duck[l] < duck_target[l])) duck[l] = duck_target[l];
        }
        for (unsigned i = 0; i < frames; ++i) busbuf[l][i] = 0;
    }
    /* voices */
    unsigned any = 0;
    for (unsigned n = 0; n < VOICES; ++n) {
        struct voice *x = &v[n];
        if (!x->on) continue;
        any = 1;
        int32_t amp = x->amp, *b = busbuf[x->part];
        if (!amp) continue;
        if (x->w1 == W_NOISE_L || x->w1 == W_NOISE_S) {
            for (unsigned i = 0; i < frames; ++i) {
                uint32_t before = x->nclk;
                x->nclk += x->ninc;
                if (x->nclk < before || !x->nval) {
                    unsigned r = x->lfsr ? x->lfsr : 1U, bit = x->nshort ? ((r ^ (r >> 6)) & 1U) : ((r ^ (r >> 1)) & 1U);
                    r = (r >> 1) | (bit << 14); x->lfsr = (uint16_t)r;
                    x->nval = (r & 1U) ? 20000 : -20000;
                }
                b[i] += (x->nval * amp) >> 15;
            }
            continue;
        }
        int32_t f = 0;
        if (x->filt) { int32_t idx = x->cut >> 8; f = chip_cut[idx < 0 ? 0 : idx > 63 ? 63 : idx]; }
        for (unsigned i = 0; i < frames; ++i) {
            int32_t s = (wave(x->w1, x->ph1) * x->mix1) >> 15;
            x->ph1 += x->inc1;
            if (x->w2) { s += (wave(x->w2, x->ph2) * x->mix2) >> 15; x->ph2 += x->inc2; }
            if (x->filt) {
                x->lp += (f * x->bp) >> 15;
                int32_t hp = s - x->lp - ((x->q * x->bp) >> 15);
                x->bp += (f * hp) >> 15;
                if (x->bp > 200000) x->bp = 200000; else if (x->bp < -200000) x->bp = -200000;
                s = x->lp;
            }
            b[i] += (s * amp) >> 15;
        }
    }
    /* parts, echo, the console's gentle output filtering */
    uint32_t mask = ECHO_LEN - 1, dly = chip_echo_samples & mask;
    int32_t gain[CHIP_PARTS], send[CHIP_PARTS];
    for (unsigned l = 0; l < CHIP_PARTS; ++l) {
        gain[l] = (int32_t)(((int64_t)chip_part_gain[l] * duck[l]) >> 16);
        send[l] = chip_echo_send[l];
    }
    for (unsigned i = 0; i < frames; ++i) {
        int32_t mono = 0, fx = 0;
        for (unsigned l = 0; l < CHIP_PARTS; ++l) {
            int32_t partv = (busbuf[l][i] * gain[l]) >> 15;
            mono += partv;
            fx += (partv * send[l]) >> 15;
        }
        int32_t d = echo_buf[(echo_w - dly) & mask];
        echo_lp += ((d - echo_lp) * 18850) >> 15;                       /* 6 kHz */
        int32_t w = fx + ((echo_lp * chip_echo_fb) >> 15);                /* feedback */
        echo_buf[echo_w & mask] = (int16_t)(w > 32767 ? 32767 : w < -32768 ? -32768 : w);
        echo_w = echo_w + 1;
        mono += (echo_lp * chip_echo_ret) >> 15;                          /* return */
        int32_t y = mono - hp_x + ((hp_y * 32373) >> 15);                 /* 90 Hz highpass */
        hp_x = mono; hp_y = y;
        if (y > 32767) y = 32767 + ((y - 32767) >> 2);
        if (y < -32768) y = -32768 + ((y + 32768) >> 2);
        if (y > 49151) y = 49151; else if (y < -49152) y = -49152;
        output[2 * i] += y << 8;
        output[2 * i + 1] += y << 8;
    }
    (void)any;
}

/* ------------------------------------------------------------ control-task helpers (not in the interrupt) */
static void live_push(unsigned kind, unsigned part, unsigned value, unsigned key)
{
    if (part >= CHIP_PARTS || live_w - live_r >= 16U) return;
    unsigned i = live_w & 15U;
    live_req_kind[i] = (uint8_t)kind; live_req_part[i] = (uint8_t)part; live_req_deg[i] = (uint8_t)value; live_req_key[i] = (uint8_t)key;
    live_w = live_w + 1;
}
void chip_live_note(unsigned part, unsigned degree) { live_push(0, part, degree, 0); }
void chip_key_down(unsigned part, unsigned key, unsigned note) { live_push(1, part, note > 127 ? 127 : note, key); }
void chip_key_up(unsigned key) { live_push(2, 0, 0, key); }
void chip_dsp_stop(void) { chip_rt.stop_req = 1; }
void chip_dsp_restart_clock(void) { step_left = 0; tick_left = 0; }

static float fexp(float x) /* e^x, any sign */
{
    int neg = x < 0; if (neg) x = -x;
    float y = 1.f + x / 1024.f;
    for (int i = 0; i < 10; ++i) y *= y;
    return neg ? 1.f / y : y;
}
static float fsin(float x)
{
    const float pi = 3.14159265f;
    while (x > pi) x -= 2.f * pi;
    while (x < -pi) x += 2.f * pi;
    float x2 = x * x;
    return x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f * (1.f - x2 / 72.f * (1.f - x2 / 110.f)))));
}
static float freq_of(int n) { float f = 8.1757989f; for (int i = 0; i < n; ++i) f *= 1.0594631f; return f; }
static int32_t note_q8_of(float hz)
{
    int n = 0; float f = 8.1757989f;
    while (n < 127 && f * 1.0594631f <= hz) { f *= 1.0594631f; ++n; }
    float frac = (hz - f) / (f * 0.0594631f);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return n * 256 + (int32_t)(frac * 255.f);
}
static int32_t cut_q8_of(float hz) /* position in chip_cut x 256 */
{
    float f = 100.f; int i = 0;
    while (i < 63 && f * 1.07404f <= hz) { f *= 1.07404f; ++i; }
    float frac = (hz - f) / (f * 0.07404f);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return i * 256 + (int32_t)(frac * 255.f);
}
static int32_t kq16(float k) { return (int32_t)(k * 65536.f); }
static const float FR = 1.f / 60.f;
static int32_t decay_k(float d) { float t = d / 3.f; if (t < .01f) t = .01f; return kq16(fexp(-FR / t)); }
static int32_t release_k(float r) { float t = r / 4.f; if (t < .005f) t = .005f; return kq16(fexp(-FR / t)); }
struct raw_patch { float A, D, S, R, rate, cents, delay; };
static struct raw_patch raw[CHIP_PATCHES];
static void patch(unsigned id, unsigned w1, float m1, unsigned w2, float m2, float ratio2, float A, float D, float S, float R, float pk,
                  float vib_rate, float vib_cents, float vib_delay)
{
    struct chip_patch *p = &chip_patches[id];
    raw[id].A = A; raw[id].D = D; raw[id].S = S; raw[id].R = R; raw[id].rate = vib_rate; raw[id].cents = vib_cents; raw[id].delay = vib_delay;
    p->w1 = (uint8_t)w1; p->w2 = (uint8_t)w2; p->mix1 = (int16_t)(m1 * 32767.f); p->mix2 = (int16_t)(m2 * 32767.f);
    p->ratio2 = (int32_t)(ratio2 * 65536.f);
    p->atk = (int16_t)(A <= FR ? 0 : (int)(A * 60.f + .999f));
    p->kd = decay_k(D); p->kr = release_k(R); p->sus = kq16(S); p->pk = (int32_t)(pk * 32767.f * 1.6f);
    p->vib_depth = (int16_t)(vib_cents * 256.f / 100.f); p->vib_delay = (int16_t)(vib_delay * 60.f); p->vib_inc = (uint16_t)(vib_rate / 60.f * 65536.f);
}
static void filter(unsigned id, float from, float to, float tau, float q)
{
    struct chip_patch *p = &chip_patches[id];
    p->filt = 1; p->cut_start = cut_q8_of(from); p->cut_end = cut_q8_of(to); p->cut_k = kq16(fexp(-FR / tau)); p->q = (int32_t)(32768.f / q);
}
void chip_dsp_init(void)
{
    recip[0] = 65536; for (int n = 1; n < 257; ++n) recip[n] = 65536U / (uint32_t)n;
    for (int n = 0; n < 130; ++n) chip_inc[n] = (uint32_t)(freq_of(n) * 97391.548f);
    float f = 100.f;
    for (int i = 0; i < 64; ++i) { float c = 2.f * fsin(3.14159265f * f / 44100.f); if (c > 1.3f) c = 1.3f; chip_cut[i] = (int32_t)(c * 32768.f); f *= 1.07404f; }
    for (int i = 0; i < 32; ++i) {
        float a = (float)i / 32.f * 6.2831853f, w = fsin(a) * .7f + fsin(a * 2.f) * .3f;
        int st = (int)(w * 7.f + (w >= 0 ? .5f : -.5f));
        chip_wave32[i] = (int16_t)(st * 24000 / 7);
        int t = i < 16 ? 15 - i : i - 16;
        chip_tri32[i] = (int16_t)(((float)t / 7.5f - 1.f) * 24000.f);
    }
    /*               id           w1     m1   w2     m2   ratio  A     D    S     R     pk    vib */
    patch(P_PULSE50,   W_P50, 1,  0,    0,   1,     0,    .3f, .7f,  .07f, .15f, 6, 22, .22f);
    patch(P_PULSE25,   W_P25, 1,  0,    0,   1,     0,    .25f, .65f, .07f, .15f, 6, 22, .22f);
    patch(P_SIDLEAD,   W_SAW, .5f, W_P25, .5f, 1.003f, 0,  .3f, .75f, .1f,  .13f, 6.5f, 26, .2f);
    filter(P_SIDLEAD, 4200, 1800, .15f, 5);
    patch(P_PULSE12,   W_P12, 1,  0,    0,   1,     0,    .14f, .35f, .05f, .14f, 0, 0, 0);
    patch(P_WAVE,      W_WAVE, 1, 0,    0,   1,     0,    .12f, .35f, .05f, .22f, 0, 0, 0);
    patch(P_TRI,       W_TRI, 1,  0,    0,   1,     0,    .1f, 1,    .03f, .5f,  0, 0, 0);
    patch(P_PULSEBASS, W_P25, .67f, W_P50, .33f, .5f, 0,  .15f, .6f, .05f, .17f * 1.5f, 0, 0, 0);
    patch(P_SIDBASS,   W_SAW, .6f, W_P50, .4f, 1,    0,    .18f, .55f, .05f, .24f, 0, 0, 0);
    filter(P_SIDBASS, 2600, 480, .06f, 7);
    patch(P_ARPCHORD,  W_P25, 1,  0,    0,   1,     0,    .8f, .6f,  .08f, .15f, 0, 0, 0);
    patch(P_PULSEPAD,  W_P25, 1,  0,    0,   1,     .08f, .6f, .75f, .2f,  .07f, 5, 10, .3f);
    patch(P_WAVEPAD,   W_WAVE, 1, 0,    0,   1,     .05f, .6f, .8f,  .2f,  .1f,  0, 0, 0);
    /* drums: the app's hits, each a stepped decay of decay x 3 held for decay x 4 */
    struct chip_drum_consts *d = &chip_dc;
    d->kick_from = note_q8_of(240); d->kick_to = note_q8_of(48);
    d->snare_from = note_q8_of(220); d->snare_to = note_q8_of(140);
    static const float tf[3] = {330, 240, 170}, tt[3] = {140, 100, 70};
    for (int i = 0; i < 3; ++i) { d->tom_from[i] = note_q8_of(tf[i]); d->tom_to[i] = note_q8_of(tt[i]); }
    d->noise_inc = 0x80000000u; d->noise_short_inc = 715827882u;
    d->noise_rate[0] = 256; d->noise_rate[1] = 192; d->noise_rate[2] = 307;
#define LV(x) ((int32_t)((x) * 32767.f * 1.6f))
    d->lvl_kick = LV(.8f); d->lvl_kick_gb = LV(.35f); d->lvl_snare = LV(.45f); d->lvl_snare_tone = LV(.14f); d->lvl_clap = LV(.38f);
    d->lvl_hat = LV(.16f); d->lvl_open = LV(.14f); d->lvl_tom = LV(.65f); d->lvl_tom_gb = LV(.3f); d->lvl_crash = LV(.24f); d->lvl_riser = LV(.22f);
#undef LV
    d->k_kick = decay_k(.3f); d->k_snare = decay_k(.27f); d->k_snare_tone = decay_k(.12f); d->k_clap = decay_k(.18f); d->k_hat = decay_k(.066f);
    d->k_open = decay_k(.36f); d->k_tom = decay_k(.36f); d->k_crash = decay_k(1.35f); d->k_release = release_k(.02f);
    d->g_kick = 24; d->g_snare = 22; d->g_snare_tone = 10; d->g_clap = 14; d->g_hat = 5; d->g_open = 29; d->g_tom = 29; d->g_crash = 108;
    static const uint8_t moods[CHIP_MOODS][8] = {{7, 0, 2, 4, 5, 7, 9, 11}, {7, 0, 2, 4, 5, 7, 9, 10}, {7, 0, 2, 3, 5, 7, 8, 10}, {7, 0, 2, 3, 5, 7, 8, 11}, {7, 0, 1, 3, 5, 7, 8, 10}};
    for (int m = 0; m < CHIP_MOODS; ++m) for (int i = 0; i < 8; ++i) chip_scale[m][i] = moods[m][i];
    for (int l = 0; l < CHIP_PARTS; ++l) { duck[l] = duck_target[l] = 65536; }
}
void chip_dsp_tempo(unsigned bpm)
{
    if (bpm < 40) bpm = 40;
    if (bpm > 240) bpm = 240;
    chip_step_samples = 44100U * 15U / bpm;
    chip_ticks_per_step_q8 = 900U * 256U / bpm;
    static const float division[3] = {.5f, .75f, 1.f};
    float sec = 60.f / (float)bpm * division[chip_song.echo_time % 3];
    if (sec < .15f) sec = .15f;
    if (sec > .37f) sec = .37f;
    chip_echo_samples = (uint32_t)(sec * 44100.f);
}
/* A part's sound with its envelope (ENV page) and vibrato (LFO page) edits; 50 leaves a setting as designed. */
static float scale2(int v, float span) { return fexp((float)(v - 50) / span * 0.6931472f); }
void chip_dsp_part_patch(unsigned part, unsigned id)
{
    if (part >= CHIP_PARTS || id >= CHIP_PATCHES) return;
    const struct raw_patch *r = &raw[id];
    const uint8_t *e = chip_song.env[part], *l = chip_song.vib[part];
    struct chip_patch p = chip_patches[id];
    float A = r->A > FR ? r->A * scale2(e[0], 12.5f) : e[0] > 50 ? (float)(e[0] - 50) / 50.f * .6f : 0.f;
    float D = r->D * scale2(e[1], 16.7f), S = e[2] == 50 ? r->S : (float)e[2] / 100.f, R = r->R * scale2(e[3], 16.7f);
    p.atk = (int16_t)(A <= FR ? 0 : (int)(A * 60.f + .999f)); p.kd = decay_k(D); p.sus = kq16(S); p.kr = release_k(R);
    float rate = (r->rate > 0 ? r->rate : 6.f) * scale2(l[0], 25.f);
    float cents = r->cents > 0 ? r->cents * (float)l[1] / 50.f : l[1] > 50 ? (float)(l[1] - 50) * .8f : 0.f;
    float delay = (r->delay > 0 ? r->delay : .2f) * scale2(l[2], 25.f);
    p.vib_depth = (int16_t)(cents * 256.f / 100.f); p.vib_delay = (int16_t)(delay * 60.f); p.vib_inc = (uint16_t)(rate / 60.f * 65536.f);
    chip_part_patch_id[part] = 255;                    /* the interrupt uses the stock sound while this one changes */
    chip_part_patch[part] = p;
    chip_part_patch_id[part] = (uint8_t)id;
}
void chip_dsp_mix(void)
{
    static const float sends[CHIP_PARTS] = {.3f, .3f, .12f, 0, .05f};
    for (int l = 0; l < CHIP_PARTS; ++l) {
        float lv = chip_song.level[l] / 100.f, g = lv * lv * 1.3f * .85f;
        chip_part_gain[l] = chip_song.mute[l] ? 0 : (int32_t)(g * 32767.f);
        chip_echo_send[l] = (int32_t)(sends[l] * chip_song.echo / 5.f * 32767.f);
    }
    chip_echo_fb = (int32_t)((.1f + chip_song.echo_fb * .06f) * 32767.f);   /* 0..10 -> .1 .. .7 */
    chip_echo_ret = (int32_t)(.35f * 32767.f);
}
