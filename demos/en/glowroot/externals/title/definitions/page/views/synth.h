/* Glowroot's sounds, generated: a soft pad per screen and short voices. */
#pragma once
#include <tsugumi/tsugumi_view.h>

void synth_start(void);
/* sin and cos of a turn fraction (1 = a full circle). */
float synth_sin(float turns);
float synth_cos(float turns);
void synth_screen(const char* screen, tsugumi_view_time at);
void synth_click(tsugumi_view_time at);
void synth_thud(tsugumi_view_time at);
void synth_buy(tsugumi_view_time at);
void synth_chime(tsugumi_view_time at);
void synth_bloom(tsugumi_view_time at);
/* Stereo float frames for the host, or NULL. */
const float* synth_render(uint32_t frames, tsugumi_view_time heard);
