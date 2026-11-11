/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * bi-1 chip synth for the M-VAVE FM-1.
 *
 * A port of the bi-1 web app: its own little sound chip (pulse waves at
 * 12.5/25/50%, a stepped 4-bit triangle, a 32-step wave channel, a SID-style
 * filtered saw, and long and short LFSR noise, with 4-bit volume moving on
 * 60 Hz ticks), its tracker echo, its five parts (lead, arp, chords, bass,
 * drums) with light and full patterns, its moods, chord progressions A/B,
 * banks and song mode.
 *
 * The sound is synthesized by the APP inside the audio interrupt and mixed
 * into the FM-1 output before the MASTER knob, so it plays on the hardware,
 * over USB and in the browser workbench alike. The composer and the panel
 * run in the control task. */
#ifndef FM1_CHIP_H
#define FM1_CHIP_H
#include <stdint.h>
#include "fm1.h"

enum { CHIP_LEAD, CHIP_ARP, CHIP_CHORDS, CHIP_BASS, CHIP_DRUMS, CHIP_PARTS };
enum { CHIP_BANKS = 8, CHIP_STEPS = 16, CHIP_MOODS = 5 };
enum { CHIP_OFF, CHIP_LIGHT, CHIP_FULL };

/* A melodic step: scale-degree offset from the bar's chord, length in steps (0 = empty). */
struct chip_note { int8_t o; uint8_t d; };
/* Every part's light and full pattern. Drum steps are bit masks of the app's
 * drum numbers: 0 kick, 1 snare, 2 clap, 3 hat, 4 open hat, 5 tom, 8 crash. */
struct chip_kit {
    struct chip_note mel[4][2][CHIP_STEPS]; /* lead, arp, chords, bass x light, full */
    uint16_t drums[2][CHIP_STEPS];
};
/* What plays: each part off, light or full, which progression, a key lift, the bank it came from. */
struct chip_state { uint8_t lv[CHIP_PARTS]; uint8_t prog; int8_t lift; int8_t bank; };

struct chip_song {
    char name[20];
    uint8_t mood, key, bpm;
    uint8_t progs[2][4];
    struct chip_kit kit;
    uint8_t patch[CHIP_PARTS];  /* three sounds per part */
    uint8_t level[CHIP_PARTS];  /* 0..100 */
    uint8_t mute[CHIP_PARTS];
    uint8_t arp_dir, arp_rate, arp_range, arp_len;
    uint8_t echo;               /* 0..10 */
    uint8_t echo_time, echo_fb; /* 1/8, dotted 1/8, 1/4; feedback 0..10 */
    uint8_t env[CHIP_PARTS][4]; /* ENV page: attack, decay, sustain, release (50 = as designed) */
    uint8_t vib[CHIP_PARTS][3]; /* LFO page: vibrato rate, depth, delay (50 = as designed) */
    uint8_t oct[CHIP_PARTS];    /* keyboard octave per part, stored +3 */
};
struct chip_bank { uint8_t used; struct chip_kit kit; struct chip_state st; };

/* Shared with the audio interrupt. The control task writes requests; the
 * interrupt applies them on bar lines, as the app does. */
struct chip_runtime {
    volatile uint8_t playing, song_mode, fill_req, stop_req;
    volatile int32_t gstep;          /* 16th steps since start */
    volatile int32_t change_at;      /* step where a requested state lands, -1 none */
    volatile uint8_t has_target;
    struct chip_state live, target;
    volatile int8_t mood_next, key_next; /* -1 none */
    volatile int32_t song_bar;
    volatile uint8_t note_on[CHIP_PARTS]; /* flashes for the screen */
};

extern struct chip_song chip_song;
extern struct chip_bank chip_banks[CHIP_BANKS];
extern struct chip_runtime chip_rt;

/* Audio interrupt: add one block of chip sound into the Q23 stereo output. */
FM1_AUDIO_CODE void chip_render(int32_t *output, unsigned frames);
/* Audio interrupt: live notes from the panel keys. */
void chip_live_note(unsigned part, unsigned degree);
/* Control task: a panel key goes down (with the MIDI note it plays) or up. */
void chip_key_down(unsigned part, unsigned key, unsigned note);
void chip_key_up(unsigned key);

/* Control task. */
void chip_init(uint32_t seed);
void chip_poll(uint32_t now);
void chip_play(int on);
void chip_play_button(void);               /* the app's play: start, wind down, stop now */
void chip_new_loop(void);
void chip_clear_part(unsigned part);
void chip_drift_cycle(void);
void chip_record_toggle(void);
void chip_record_down(unsigned part, unsigned key, unsigned note);
void chip_record_up(unsigned key);
extern uint8_t chip_drift, chip_recording, chip_winding;
void chip_cycle_part(unsigned part);        /* off -> light -> full -> off, on the next bar */
void chip_toggle_prog(void);
void chip_toggle_lift(void);
void chip_fill(void);
void chip_bank_play(unsigned bank);
void chip_bank_save(unsigned bank);
void chip_bank_delete(unsigned bank);
void chip_song_mode(void);
void chip_new_patterns(unsigned part);
void chip_new_progressions(void);
void chip_remix(void);
int chip_undo(void);
void chip_mood(int delta);
void chip_key(int delta);
void chip_tempo(int delta);
void chip_patch(unsigned part, int delta);
void chip_level(unsigned part, int delta);
void chip_mute(unsigned part);
void chip_arp_style(int delta);
void chip_echo(int delta);

void chip_refresh_sounds(void);
void chip_env_edit(unsigned part, unsigned param, int delta);
void chip_vib_edit(unsigned part, unsigned param, int delta);
void chip_echo_time(int delta);
void chip_echo_feedback(int delta);
void chip_arp_param(unsigned param, int delta);
void chip_prog_chord(unsigned prog, unsigned slot, int delta);
void chip_step_toggle(unsigned part, unsigned which, unsigned step, int degree, unsigned drum);
void chip_step_pitch(unsigned part, unsigned which, unsigned step, int delta);
void chip_step_length(unsigned part, unsigned which, unsigned step, int delta);
void chip_pattern_copy(unsigned part, unsigned to);
int chip_store_save(unsigned slot);
int chip_store_load(unsigned slot);
const char *chip_store_name(unsigned slot);
extern uint8_t chip_slot;
const char *chip_part_name(unsigned part);
const char *chip_patch_name(unsigned part);
const char *chip_mood_name(void);
const char *chip_key_name(void);
const char *chip_arp_name(void);
extern const char *chip_notice;
extern uint32_t chip_notice_until;
extern uint32_t chip_revision;
#endif
