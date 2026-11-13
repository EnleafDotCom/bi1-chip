/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm_patch.h"
#ifndef FM1_CHIP_ONLY
#define FM1_CHIP_ONLY 1
#endif
#if FM1_CHIP_ONLY
/* bi-1 firmware: the chip synth makes every sound and the stock FM engine is
 * silent, so one valid voice (factory PIANO 1) stands in for the 128-voice
 * bank. This keeps about 10 KB of Flash free for bi-1. */
static const uint8_t factory_voice[128] = {
    95,29,20,50,99,95,0,0,41,0,19,0,115,24,79,2,
    0,95,20,20,50,99,95,0,0,0,0,0,0,3,0,99,
    2,0,95,29,20,50,99,95,0,0,0,0,0,0,59,24,
    89,2,0,95,20,20,50,99,95,0,0,0,0,0,0,59,
    8,99,2,0,95,50,35,78,99,75,0,0,0,0,0,0,
    59,28,58,28,0,96,25,25,67,99,75,0,0,0,0,0,
    0,83,8,99,2,0,94,67,95,60,50,50,50,50,4,6,
    15,33,0,0,40,24,80,73,65,78,79,32,49,32,32,32
};
FM1_DSP_CODE const uint8_t *groove_fm_factory_raw(unsigned program)
{
    (void)program;
    return factory_voice;
}
#else
#include "fm_factory.inc"

/* Expand once during startup, before the audio owners are running. MIDI
 * program changes then read SRAM without decompression or heap allocation. */
static uint8_t factory_raw[FM1_FM_PROGRAMS*128];
static unsigned factory_ready;
static FM1_DSP_CODE void factory_init(void)
{
    if (factory_ready) return;
    uint8_t *out=factory_raw;
    for (unsigned at=0;at<sizeof factory_packed;) {
        unsigned token=factory_packed[at++];
        if (token<128) *out++=(uint8_t)token;
        else {
            unsigned low=factory_packed[at++],distance=low&127U;
            if (low&128U) distance|=(unsigned)factory_packed[at++]<<7;
            for (unsigned n=token-128+3;n;--n) { *out=out[-(int)distance]; ++out; }
        }
    }
    factory_ready=1;
}
FM1_DSP_CODE const uint8_t *groove_fm_factory_raw(unsigned program)
{
    factory_init();
    return factory_raw+(program%FM1_FM_PROGRAMS)*128;
}
#endif
FM1_DSP_CODE void groove_fm_factory(uint8_t p[156], unsigned program)
{
    groove_fm_unpack(p,groove_fm_factory_raw(program));
}
