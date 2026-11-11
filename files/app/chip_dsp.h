/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * bi-1 chip synth: tables shared between the control task (which fills them)
 * and the audio interrupt (which reads them). */
#ifndef FM1_CHIP_DSP_H
#define FM1_CHIP_DSP_H
#include <stdint.h>
#include "chip.h"

/* The app's eleven sounds. */
enum { P_PULSE50, P_PULSE25, P_SIDLEAD, P_PULSE12, P_WAVE, P_TRI, P_PULSEBASS, P_SIDBASS, P_ARPCHORD, P_PULSEPAD, P_WAVEPAD,
       CHIP_PATCHES };
struct chip_patch {
    uint8_t w1, w2, filt, pad;
    int16_t mix1, mix2;          /* Q15 */
    int32_t ratio2;              /* Q16 */
    int16_t atk, vib_depth, vib_delay; uint16_t vib_inc;
    int32_t kd, kr, sus;         /* Q16 */
    int32_t pk;                  /* Q15 */
    int32_t cut_start, cut_end, cut_k, q;
};
struct chip_drum_consts {
    int32_t kick_from, kick_to, snare_from, snare_to, tom_from[3], tom_to[3]; /* note x 256 */
    uint32_t noise_inc, noise_short_inc;                                       /* rate 1 */
    int32_t noise_rate[3];                                                     /* Q8 per kit */
    int32_t lvl_kick, lvl_kick_gb, lvl_snare, lvl_snare_tone, lvl_clap, lvl_hat, lvl_open, lvl_tom, lvl_tom_gb, lvl_crash, lvl_riser; /* Q15 */
    int32_t k_kick, k_snare, k_snare_tone, k_clap, k_hat, k_open, k_tom, k_crash, k_release; /* Q16 per tick */
    int16_t g_kick, g_snare, g_snare_tone, g_clap, g_hat, g_open, g_tom, g_crash;           /* gate ticks */
};
extern struct chip_patch chip_patches[CHIP_PATCHES];
extern struct chip_drum_consts chip_dc;
extern uint32_t chip_step_samples, chip_ticks_per_step_q8, chip_echo_samples;
extern int32_t chip_part_gain[CHIP_PARTS], chip_echo_send[CHIP_PARTS];
extern uint8_t chip_scale[CHIP_MOODS][8];

void chip_dsp_init(void);            /* control task, before audio */
void chip_dsp_tempo(unsigned bpm);   /* control task */
void chip_dsp_mix(void);             /* control task: levels, mutes, echo */
void chip_dsp_part_patch(unsigned part, unsigned patch_id); /* control task: a part's ENV/LFO-edited sound */
void chip_dsp_stop(void);            /* control task: release everything */
void chip_dsp_restart_clock(void);   /* control task: next block starts a step */
#endif
