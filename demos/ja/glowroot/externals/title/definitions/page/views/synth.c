#include "synth.h"

enum { TABLE = 2048, MAX_FRAMES = 16384, VOICES = 32, PAD_NOTES = 6 };
static const float kRate = TSUGUMI_VIEW_SAMPLE_RATE;

static float table[TABLE + 1];
static float out[2 * MAX_FRAMES];

float synth_sin(float turns) {
  float f = turns - (float)(int)turns;
  if (f < 0) f += 1;
  const float at = f * TABLE;
  int i = (int)at;
  if (i >= TABLE) i = TABLE - 1;
  const float t = at - (float)i;
  return table[i] + (table[i + 1] - table[i]) * t;
}
float synth_cos(float turns) { return synth_sin(turns + 0.25f); }

void synth_start(void) {
  /* sin by the recurrence s[k+1] = 2 cos(d) s[k] - s[k-1], in double. */
  const double d = 6.283185307179586 / TABLE;
  double c = 1, term = 1;
  for (int n = 1; n < 12; ++n) { /* cos(d) by its series */
    term *= -d * d / ((2.0 * n - 1) * (2.0 * n));
    c += term;
  }
  double previous = 0, current = 0, s1 = 0;
  /* sin(d) by its series */
  term = d;
  s1 = d;
  for (int n = 1; n < 12; ++n) {
    term *= -d * d / ((2.0 * n) * (2.0 * n + 1));
    s1 += term;
  }
  table[0] = 0;
  current = s1;
  for (int k = 1; k <= TABLE; ++k) {
    table[k] = (float)current;
    const double next = 2 * c * current - previous;
    previous = current;
    current = next;
  }
  table[TABLE] = 0;
}

static float note(float midi) {
  /* 440 * 2^((midi - 69) / 12), with 2^x from its integer part and a series. */
  const float x = (midi - 69) / 12;
  int whole = (int)(x < 0 ? x - 1 : x);
  const float frac = x - (float)whole;
  float p = 1, term = 1;
  for (int n = 1; n < 8; ++n) {
    term *= frac * 0.6931472f / (float)n;
    p += term;
  }
  float f = 440 * p;
  for (; whole > 0; --whole) f *= 2;
  for (; whole < 0; ++whole) f /= 2;
  return f;
}

/* ---- the pad ------------------------------------------------------------ */

typedef struct { const char* screen; float notes[PAD_NOTES]; } chord;
static const chord kChords[] = {
    {"title", {45, 52, 57, 60, 64, 0}},   {"grow", {48, 55, 60, 64, 67, 71}},
    {"skills", {41, 48, 57, 60, 64, 69}}, {"replant", {50, 57, 62, 65, 69, 0}},
    {"ending", {43, 50, 55, 59, 62, 66}},
};
static int chord_from = -1, chord_to = 0;
static tsugumi_view_time chord_changed;
static float pad_phase[2][PAD_NOTES][2];
static const float kFade = 2.5f * TSUGUMI_VIEW_SAMPLE_RATE;

static int same(const char* a, const char* b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}

void synth_screen(const char* screen, tsugumi_view_time at) {
  for (int i = 0; i < (int)(sizeof kChords / sizeof kChords[0]); ++i) {
    if (!same(kChords[i].screen, screen) || i == chord_to) continue;
    chord_from = chord_to;
    chord_to = i;
    chord_changed = at;
    for (int n = 0; n < PAD_NOTES; ++n)
      for (int k = 0; k < 2; ++k) {
        pad_phase[0][n][k] = pad_phase[1][n][k];
        pad_phase[1][n][k] = 0;
      }
  }
}

/* ---- voices --------------------------------------------------------------- */

typedef struct {
  tsugumi_view_time start;
  float freq, glide, amp, decay, phase, env, length;
  int bell, live;
} voice;
static voice voices[VOICES];
static unsigned voice_next;
static tsugumi_view_time last_heard;

static void play(tsugumi_view_time at, float delay_s, float freq, float glide, float amp,
                 float seconds, int bell) {
  voice* v = &voices[voice_next++ % VOICES];
  v->start = (at > last_heard ? at : last_heard) + (tsugumi_view_time)(delay_s * kRate);
  v->freq = freq;
  v->glide = glide;
  v->amp = amp;
  v->decay = 1 - 1 / (seconds * kRate / 4);
  v->phase = 0;
  v->env = 1;
  v->length = seconds * kRate;
  v->bell = bell;
  v->live = 1;
}

void synth_click(tsugumi_view_time at) { play(at, 0, 1046, -0.25f, 0.10f, 0.09f, 0); }
void synth_thud(tsugumi_view_time at) { play(at, 0, 196, -0.1f, 0.08f, 0.10f, 0); }
void synth_buy(tsugumi_view_time at) {
  play(at, 0, note(76), 0, 0.09f, 0.16f, 0);
  play(at, 0.07f, note(83), 0, 0.09f, 0.22f, 0);
}
void synth_chime(tsugumi_view_time at) {
  play(at, 0, note(72), 0, 0.10f, 2.4f, 1);
  play(at, 0.16f, note(76), 0, 0.08f, 2.2f, 1);
  play(at, 0.32f, note(79), 0, 0.08f, 2.0f, 1);
  play(at, 0.48f, note(84), 0, 0.07f, 2.6f, 1);
}
void synth_bloom(tsugumi_view_time at) {
  static const float notes[] = {67, 71, 74, 78, 79, 83, 86, 91};
  for (int i = 0; i < 8; ++i) play(at, 0.14f * (float)i, note(notes[i]), 0, 0.07f, 2.8f, 1);
}

const float* synth_render(uint32_t frames, tsugumi_view_time heard) {
  if (frames > MAX_FRAMES) return 0;
  last_heard = heard;
  for (uint32_t i = 0; i < 2 * frames; ++i) out[i] = 0;

  /* the pad: two layers while the chord changes */
  for (int layer = 0; layer < 2; ++layer) {
    const int c = layer ? chord_to : chord_from;
    if (c < 0) continue;
    for (int n = 0; n < PAD_NOTES; ++n) {
      const float midi = kChords[c].notes[n];
      if (midi <= 0) continue;
      const float f = note(midi);
      for (int k = 0; k < 2; ++k) {
        const float step = f * (k ? 1.003f : 0.997f) / kRate;
        float phase = pad_phase[layer][n][k];
        for (uint32_t i = 0; i < frames; ++i) {
          const tsugumi_view_time t = heard + i;
          const float since = t > chord_changed ? (float)(t - chord_changed) : 0;
          float mix = since / kFade;
          if (mix > 1) mix = 1;
          const float gain = layer ? mix : 1 - mix;
          if (gain <= 0) {
            phase += step;
            continue;
          }
          const float swell = 0.6f + 0.4f * synth_sin((float)t / kRate * (0.05f + 0.013f * (float)n));
          const float s = synth_sin(phase) * 0.018f * gain * swell;
          out[2 * i + k] += s;
          out[2 * i + (k ^ 1)] += s * 0.6f;
          phase += step;
          if (phase > 1) phase -= 1;
        }
        pad_phase[layer][n][k] = phase;
      }
    }
  }

  /* the voices */
  for (int j = 0; j < VOICES; ++j) {
    voice* v = &voices[j];
    if (!v->live) continue;
    for (uint32_t i = 0; i < frames; ++i) {
      const tsugumi_view_time t = heard + i;
      if (t < v->start) continue;
      const float age = (float)(t - v->start);
      if (age >= v->length) {
        v->live = 0;
        break;
      }
      const float attack = age < 120 ? age / 120 : 1;
      const float f = v->freq * (1 + v->glide * age / v->length);
      float s = synth_sin(v->phase);
      if (v->bell) s = 0.7f * s + 0.2f * synth_sin(v->phase * 2.76f) + 0.1f * synth_sin(v->phase * 5.4f);
      s *= v->amp * v->env * attack;
      out[2 * i] += s;
      out[2 * i + 1] += s;
      v->env *= v->decay;
      v->phase += f / kRate;
      if (v->phase > 64) v->phase -= 64;
    }
  }

  for (uint32_t i = 0; i < 2 * frames; ++i) { /* a soft limit */
    const float x = out[i];
    out[i] = x * (27 + x * x) / (27 + 9 * x * x);
  }
  return out;
}
