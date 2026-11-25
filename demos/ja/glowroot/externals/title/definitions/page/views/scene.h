/* Reading the Scene messages (scene.fbs) and writing Event messages,
 * without a FlatBuffers library: the view has no libc. */
#pragma once
#include <stdint.h>

typedef struct {
  const uint8_t* data;
  uint32_t size;
} glow_str;

typedef struct {
  glow_str text;
  float x, y, size;
  uint32_t color;
  uint8_t align;
  double value;
  uint8_t counter;
} glow_label;

typedef struct {
  glow_str id, label, detail;
  float x, y, w, h;
  uint8_t enabled, style;
} glow_button;

typedef struct {
  glow_str id, label, detail;
  float x, y;
  uint8_t state;
  int16_t parent;
  uint32_t color;
} glow_node;

typedef struct {
  uint8_t kind;
  float x, y;
} glow_fx;

enum { GLOW_MAX_LABELS = 64, GLOW_MAX_BUTTONS = 32, GLOW_MAX_NODES = 32, GLOW_MAX_FX = 16 };

typedef struct {
  glow_str screen;
  uint8_t reply[16];
  uint32_t label_count, button_count, node_count, fx_count;
  glow_label labels[GLOW_MAX_LABELS];
  glow_button buttons[GLOW_MAX_BUTTONS];
  glow_node nodes[GLOW_MAX_NODES];
  glow_fx fx[GLOW_MAX_FX];
  float plant, bloom;
} glow_scene;

/* Parses buffer into scene; the strings point into buffer. 0 on success. */
int glow_scene_read(const uint8_t* buffer, uint32_t size, glow_scene* scene);
/* Writes an Event { button, x, y } into out (capacity bytes); returns its size or 0. */
uint32_t glow_event_write(uint8_t* out, uint32_t capacity, glow_str button, float x, float y);
int glow_str_eq(glow_str a, const char* b);
