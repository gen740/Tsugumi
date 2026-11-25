#include "scene.h"

/* A bounds-checked FlatBuffers reader for the few shapes scene.fbs uses. */
typedef struct {
  const uint8_t* data;
  uint32_t size;
} buf;

static int in(buf b, uint32_t at, uint32_t n) { return at <= b.size && n <= b.size - at; }
static uint32_t u32(buf b, uint32_t at) {
  return (uint32_t)b.data[at] | (uint32_t)b.data[at + 1] << 8 | (uint32_t)b.data[at + 2] << 16 |
         (uint32_t)b.data[at + 3] << 24;
}
static uint16_t u16(buf b, uint32_t at) { return (uint16_t)(b.data[at] | b.data[at + 1] << 8); }

/* The position of field id of the table at t, or 0 when absent. */
static uint32_t field(buf b, uint32_t t, uint32_t id) {
  if (!in(b, t, 4)) return 0;
  const int32_t back = (int32_t)u32(b, t);
  const int64_t vt = (int64_t)t - back;
  if (vt < 0 || !in(b, (uint32_t)vt, 4)) return 0;
  const uint16_t vt_size = u16(b, (uint32_t)vt);
  const uint32_t slot = 4 + 2 * id;
  if (slot + 2 > vt_size || !in(b, (uint32_t)vt, vt_size)) return 0;
  const uint16_t at = u16(b, (uint32_t)vt + slot);
  return at ? t + at : 0;
}
static uint32_t ref(buf b, uint32_t at) {
  if (!at || !in(b, at, 4)) return 0;
  const uint32_t target = at + u32(b, at);
  return target > at && in(b, target, 4) ? target : 0;
}
static float f32(buf b, uint32_t t, uint32_t id, float otherwise) {
  const uint32_t at = field(b, t, id);
  if (!at || !in(b, at, 4)) return otherwise;
  union { uint32_t u; float f; } v = {u32(b, at)};
  return v.f;
}
static double f64(buf b, uint32_t t, uint32_t id) {
  const uint32_t at = field(b, t, id);
  if (!at || !in(b, at, 8)) return 0;
  union { uint64_t u; double d; } v = {(uint64_t)u32(b, at) | (uint64_t)u32(b, at + 4) << 32};
  return v.d;
}
static uint32_t scalar(buf b, uint32_t t, uint32_t id, uint32_t bytes, uint32_t otherwise) {
  const uint32_t at = field(b, t, id);
  if (!at || !in(b, at, bytes)) return otherwise;
  return bytes == 1 ? b.data[at] : bytes == 2 ? u16(b, at) : u32(b, at);
}
static glow_str str(buf b, uint32_t t, uint32_t id) {
  glow_str s = {0, 0};
  const uint32_t at = ref(b, field(b, t, id));
  if (!at) return s;
  const uint32_t n = u32(b, at);
  if (!in(b, at + 4, n)) return s;
  s.data = b.data + at + 4;
  s.size = n;
  return s;
}
/* A vector of tables: count and the position of its first offset. */
static uint32_t vec(buf b, uint32_t t, uint32_t id, uint32_t* first) {
  const uint32_t at = ref(b, field(b, t, id));
  if (!at) return 0;
  const uint32_t n = u32(b, at);
  if (!in(b, at + 4, n * 4)) return 0;
  *first = at + 4;
  return n;
}

int glow_scene_read(const uint8_t* data, uint32_t size, glow_scene* s) {
  const buf b = {data, size};
  if (!in(b, 0, 8)) return -1;
  const uint32_t t = u32(b, 0);
  if (!in(b, t, 4)) return -1;
  s->screen = str(b, t, 0);
  const uint32_t reply = field(b, t, 1);
  for (uint32_t i = 0; i < 16; ++i) s->reply[i] = reply && in(b, reply, 16) ? data[reply + i] : 0;
  uint32_t first = 0, n = vec(b, t, 2, &first);
  s->label_count = 0;
  for (uint32_t i = 0; i < n && s->label_count < GLOW_MAX_LABELS; ++i) {
    const uint32_t e = ref(b, first + 4 * i);
    if (!e) continue;
    glow_label* l = &s->labels[s->label_count++];
    l->text = str(b, e, 0);
    l->x = f32(b, e, 1, 0);
    l->y = f32(b, e, 2, 0);
    l->size = f32(b, e, 3, 24);
    l->color = scalar(b, e, 4, 4, 0xffffffffu);
    l->align = (uint8_t)scalar(b, e, 5, 1, 0);
    l->value = f64(b, e, 6);
    l->counter = (uint8_t)scalar(b, e, 7, 1, 0);
  }
  n = vec(b, t, 3, &first);
  s->button_count = 0;
  for (uint32_t i = 0; i < n && s->button_count < GLOW_MAX_BUTTONS; ++i) {
    const uint32_t e = ref(b, first + 4 * i);
    if (!e) continue;
    glow_button* x = &s->buttons[s->button_count++];
    x->id = str(b, e, 0);
    x->label = str(b, e, 1);
    x->detail = str(b, e, 2);
    x->x = f32(b, e, 3, 0);
    x->y = f32(b, e, 4, 0);
    x->w = f32(b, e, 5, 0);
    x->h = f32(b, e, 6, 0);
    x->enabled = (uint8_t)scalar(b, e, 7, 1, 1);
    x->style = (uint8_t)scalar(b, e, 8, 1, 0);
  }
  n = vec(b, t, 4, &first);
  s->node_count = 0;
  for (uint32_t i = 0; i < n && s->node_count < GLOW_MAX_NODES; ++i) {
    const uint32_t e = ref(b, first + 4 * i);
    if (!e) continue;
    glow_node* x = &s->nodes[s->node_count++];
    x->id = str(b, e, 0);
    x->label = str(b, e, 1);
    x->detail = str(b, e, 2);
    x->x = f32(b, e, 3, 0);
    x->y = f32(b, e, 4, 0);
    x->state = (uint8_t)scalar(b, e, 5, 1, 0);
    x->parent = (int16_t)scalar(b, e, 6, 2, 0xffffu);
    x->color = scalar(b, e, 7, 4, 0xffffffffu);
  }
  n = vec(b, t, 5, &first);
  s->fx_count = 0;
  for (uint32_t i = 0; i < n && s->fx_count < GLOW_MAX_FX; ++i) {
    const uint32_t e = ref(b, first + 4 * i);
    if (!e) continue;
    glow_fx* x = &s->fx[s->fx_count++];
    x->kind = (uint8_t)scalar(b, e, 0, 1, 0);
    x->x = f32(b, e, 1, 0);
    x->y = f32(b, e, 2, 0);
  }
  s->plant = f32(b, t, 6, 0);
  s->bloom = f32(b, t, 7, 0);
  return 0;
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

/* Layout: root offset, vtable (3 fields), table {soffset, x, y, button}, string. */
uint32_t glow_event_write(uint8_t* out, uint32_t capacity, glow_str button, float x, float y) {
  const uint32_t total = 36 + button.size + 1;
  if (capacity < total + 3) return 0;
  __builtin_memset(out, 0, total + 3);
  put32(out, 16);                /* the table */
  put16(out + 4, 10);            /* vtable size */
  put16(out + 6, 16);            /* table size */
  put16(out + 8, 12);            /* button */
  put16(out + 10, 4);            /* x */
  put16(out + 12, 8);            /* y */
  put32(out + 16, 16 - 4);       /* table - vtable */
  union { float f; uint32_t u; } fx = {x}, fy = {y};
  put32(out + 20, fx.u);
  put32(out + 24, fy.u);
  put32(out + 28, 32 - 28);      /* the string, relative to this field */
  put32(out + 32, button.size);
  if (button.size) __builtin_memcpy(out + 36, button.data, button.size);
  return (total + 3) & ~3u;
}

int glow_str_eq(glow_str a, const char* b) {
  uint32_t i = 0;
  for (; b[i]; ++i)
    if (i >= a.size || a.data[i] != (uint8_t)b[i]) return 0;
  return i == a.size;
}
