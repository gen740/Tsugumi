/* Glowroot's screen: draws the Scenes the page Cards send, sends an Event when
 * a button or node is pressed, and plays the sounds. It holds no game rules:
 * it only animates between scenes (counters, the plant, particles). */
#include <tsugumi/dawn_font.h>
#include <tsugumi/dawn_helpers.h>

#include "scene.h"
#include "synth.h"

enum { MAX_QUADS = 6000, MAX_PARTICLES = 400, MAX_RINGS = 16, SCENE_BYTES = 1u << 18 };
static const float kWidth = 1280, kHeight = 720;

typedef struct { float x, y, w, h, r, g, b, a, u0, v0, u1, v1, radius, mode, soft, extra; } quad;
enum { SHAPE = 0, GLYPH = 1, GRADIENT = 2, RING = 3, STAR = 4 };

static WGPUDevice device;
static WGPUQueue queue;
static WGPURenderPipeline pipeline;
static WGPUBuffer quads_buffer, uniform_buffer;
static WGPUBindGroup bind_group;
static const tsugumi_dawn_font_metrics* font;
static uint32_t width, height;
static float scale = 1;

static quad quads[MAX_QUADS];
static uint32_t quad_count;

static uint8_t scene_bytes[2][SCENE_BYTES];
static uint32_t scene_slot;
static glow_scene scene;
static int have_scene;
static char screen_name[16];
static double counters[GLOW_MAX_LABELS];
static float shown_plant, shown_bloom;

static float pointer_x = -1, pointer_y = -1;
static int hover_button = -1, hover_node = -1;
static double pressed_at[GLOW_MAX_BUTTONS + GLOW_MAX_NODES];
static double now_s, last_s, screen_since_s;

typedef struct { float x, y, vx, vy, life, max, size; uint32_t color; uint8_t kind; } particle;
static particle particles[MAX_PARTICLES];
static uint32_t particle_next;
typedef struct { float x, y, t0, span, radius; uint32_t color; } ring;
static ring rings[MAX_RINGS];
static uint32_t ring_next;
static uint32_t random_state = 0x9e3779b9u;

static float rnd(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return (float)(random_state & 0xffffff) / 16777216.0f;
}

/* ---- drawing ----------------------------------------------------------- */

static void color_of(uint32_t rgba, float alpha, quad* q) {
  q->r = (float)(rgba >> 24 & 255) / 255.0f;
  q->g = (float)(rgba >> 16 & 255) / 255.0f;
  q->b = (float)(rgba >> 8 & 255) / 255.0f;
  q->a = (float)(rgba & 255) / 255.0f * alpha;
}

static quad* push(float x, float y, float w, float h, uint32_t rgba, float alpha, float mode) {
  if (quad_count >= MAX_QUADS || alpha <= 0.002f) return 0;
  quad* q = &quads[quad_count++];
  __builtin_memset(q, 0, sizeof *q);
  q->x = x;
  q->y = y;
  q->w = w;
  q->h = h;
  q->mode = mode;
  color_of(rgba, alpha, q);
  return q;
}

static void box(float x, float y, float w, float h, float radius, uint32_t rgba, float alpha) {
  quad* q = push(x, y, w, h, rgba, alpha, SHAPE);
  if (q) q->radius = radius;
}

static void glow(float cx, float cy, float radius, float soft, uint32_t rgba, float alpha) {
  quad* q = push(cx - radius, cy - radius, 2 * radius, 2 * radius, rgba, alpha, SHAPE);
  if (q) {
    q->radius = radius;
    q->soft = soft;
  }
}

static void circle(float cx, float cy, float radius, uint32_t rgba, float alpha) {
  box(cx - radius, cy - radius, 2 * radius, 2 * radius, radius, rgba, alpha);
}

static void ring_shape(float cx, float cy, float radius, float thickness, uint32_t rgba, float alpha) {
  quad* q = push(cx - radius, cy - radius, 2 * radius, 2 * radius, rgba, alpha, RING);
  if (q) {
    q->radius = radius;
    q->extra = thickness;
  }
}

static void star(float cx, float cy, float radius, uint32_t rgba, float alpha) {
  push(cx - radius, cy - radius, 2 * radius, 2 * radius, rgba, alpha, STAR);
}

static void gradient(uint32_t top, uint32_t bottom) {
  quad* q = push(0, 0, kWidth, kHeight, top, 1, GRADIENT);
  if (!q) return;
  quad b;
  color_of(bottom, 1, &b);
  q->u0 = b.r;
  q->v0 = b.g;
  q->u1 = b.b;
  q->v1 = b.a;
}

/* ---- text ---------------------------------------------------------------- */

static uint32_t utf8_next(const uint8_t* s, uint32_t n, uint32_t* i) {
  const uint8_t c = s[*i];
  uint32_t cp = c, extra = 0;
  if (c >= 0xf0) cp = c & 7, extra = 3;
  else if (c >= 0xe0) cp = c & 15, extra = 2;
  else if (c >= 0xc0) cp = c & 31, extra = 1;
  ++*i;
  while (extra-- && *i < n) cp = cp << 6 | (s[(*i)++] & 63);
  return cp;
}

/* Drawn as shapes: the font's Windows-31J set has neither. */
static const uint32_t kStar = 0x2726; /* ✦ */
static const uint32_t kDot = 0x00b7;  /* · */

static float text_width(const uint8_t* s, uint32_t n, float size) {
  const float k = size / (float)font->pixel_size;
  float w = 0;
  for (uint32_t i = 0; i < n;) {
    const uint32_t cp = utf8_next(s, n, &i);
    w += cp == kStar ? size * 0.9f : cp == kDot ? size * 0.5f : tsugumi_dawn_font_glyph(cp)->advance * k;
  }
  return w;
}

/* y is the middle of the line. */
static void text(const uint8_t* s, uint32_t n, float x, float y, float size, uint32_t rgba,
                 float alpha, int align) {
  if (!n) return;
  const float k = size / (float)font->pixel_size;
  const float w = text_width(s, n, size);
  float pen = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
  const float baseline = y + (float)(font->ascent - font->descent) * k / 2;
  for (uint32_t i = 0; i < n;) {
    const uint32_t cp = utf8_next(s, n, &i);
    if (cp == kStar) {
      star(pen + size * 0.45f, y, size * 0.36f, 0xffd77aff, alpha);
      pen += size * 0.9f;
      continue;
    }
    if (cp == kDot) {
      circle(pen + size * 0.25f, y, size * 0.08f, rgba, alpha);
      pen += size * 0.5f;
      continue;
    }
    const tsugumi_dawn_glyph* g = tsugumi_dawn_font_glyph(cp);
    if (g->width && g->height) {
      quad* q = push(pen + (float)g->left * k, baseline + (float)g->top * k, (float)g->width * k,
                     (float)g->height * k, rgba, alpha, GLYPH);
      if (q) {
        q->u0 = (float)g->x / (float)font->atlas_width;
        q->v0 = (float)g->y / (float)font->atlas_height;
        q->u1 = (float)(g->x + g->width) / (float)font->atlas_width;
        q->v1 = (float)(g->y + g->height) / (float)font->atlas_height;
      }
    }
    pen += g->advance * k;
  }
}

static void text_str(glow_str s, float x, float y, float size, uint32_t rgba, float alpha, int align) {
  text(s.data, s.size, x, y, size, rgba, alpha, align);
}

/* The same short numbers as the Cards: 950, 12.3K, 4.56M, then 1.2e15. */
static uint32_t format_number(double v, char* out) {
  uint32_t n = 0;
  if (!(v >= 0)) v = 0;
  static const char units[] = "KMBT";
  int unit = -1;
  double scaled = v;
  if (v >= 1000) {
    while (scaled >= 1000 && unit < 3) scaled /= 1000, ++unit;
  }
  if (scaled >= 1000) { /* d.de+XX */
    int exp = 0;
    while (v >= 10) v /= 10, ++exp;
    const int tenths = (int)(v * 10);
    out[n++] = (char)('0' + tenths / 10);
    out[n++] = '.';
    out[n++] = (char)('0' + tenths % 10);
    out[n++] = 'e';
    out[n++] = '+';
    out[n++] = (char)('0' + exp / 10);
    out[n++] = (char)('0' + exp % 10);
    return n;
  }
  const int digits = unit < 0 ? 0 : scaled < 10 ? 2 : scaled < 100 ? 1 : 0;
  double step = 1;
  for (int i = 0; i < digits; ++i) step *= 10;
  unsigned long long fixed = (unsigned long long)(scaled * step);
  char rev[24];
  uint32_t r = 0;
  for (int i = 0; i < digits; ++i) rev[r++] = (char)('0' + fixed % 10), fixed /= 10;
  if (digits) rev[r++] = '.';
  do rev[r++] = (char)('0' + fixed % 10), fixed /= 10; while (fixed);
  while (r) out[n++] = rev[--r];
  if (unit >= 0) out[n++] = units[unit];
  return n;
}

/* A counter's text with "{}" replaced by its shown value. */
static void counter_text(const glow_label* l, double value, float alpha) {
  uint8_t out[256];
  uint32_t n = 0;
  for (uint32_t i = 0; i < l->text.size && n < 200; ++i) {
    if (i + 1 < l->text.size && l->text.data[i] == '{' && l->text.data[i + 1] == '}') {
      char digits[24];
      const uint32_t d = format_number(value, digits);
      for (uint32_t j = 0; j < d; ++j) out[n++] = (uint8_t)digits[j];
      ++i;
    } else {
      out[n++] = l->text.data[i];
    }
  }
  text(out, n, l->x, l->y, l->size, l->color, alpha, l->align);
}

/* ---- the plant ------------------------------------------------------------- */

static float wave(float t) { return synth_sin(t); }

static void plant(float cx, float ground, float size, float bloom, float alpha) {
  const float t = (float)now_s;
  const float height_px = 40 + 300 * size;
  const float sway = 6 * size * wave(t * 0.35f);
  /* roots under the ground */
  const int roots = 2 + (int)(size * 10);
  for (int i = 0; i < roots; ++i) {
    const float spread = ((float)i / (float)(roots - 1) - 0.5f) * 2;
    const float length = 30 + 70 * size * (0.6f + 0.4f * wave((float)i * 1.7f));
    for (int s = 0; s < 6; ++s) {
      const float f = (float)s / 6.0f;
      box(cx + spread * (10 + 90 * size) * f - 2, ground + 4 + length * f, 4 * (1 - f) + 1.5f,
          length / 6 + 2, 2, 0xc9a46bff, alpha * (0.55f - 0.3f * f));
    }
  }
  /* the stem and its leaves */
  const int pieces = 14;
  for (int s = 0; s < pieces; ++s) {
    const float f = (float)s / (float)pieces;
    const float x = cx + sway * f * f;
    box(x - 4, ground - height_px * (f + 1.0f / pieces), 8, height_px / pieces + 2, 4, 0x6fc36aff, alpha);
  }
  const int leaves = 1 + (int)(size * 7);
  for (int i = 0; i < leaves; ++i) {
    const float f = 0.25f + 0.65f * (float)i / (float)(leaves > 1 ? leaves - 1 : 1);
    const float side = i % 2 ? 1.0f : -1.0f;
    const float x = cx + sway * f * f;
    const float y = ground - height_px * f;
    const float len = 26 + 46 * size * (1.1f - f * 0.5f);
    const float flutter = 3 * wave(t * 0.9f + (float)i);
    box(side > 0 ? x + 2 : x - 2 - len, y - 9 + flutter, len, 18, 9, 0x8ee07aff, alpha * 0.95f);
  }
  /* the light at its tip */
  const float tx = cx + sway, ty = ground - height_px;
  const float pulse = 1 + 0.06f * wave(t * 2.1f);
  glow(tx, ty, (26 + 34 * size) * pulse, 70, 0xffe9a0ff, alpha * 0.35f);
  circle(tx, ty, (10 + 12 * size) * pulse, 0xfff4c8ff, alpha);
  if (bloom > 0) {
    const int petals = 7;
    for (int i = 0; i < petals; ++i) {
      const float a = (float)i / petals + t * 0.03f;
      const float px = tx + synth_cos(a) * 46 * bloom, py = ty + synth_sin(a) * 46 * bloom;
      circle(px, py, 26 * bloom, 0xffa8e0ff, alpha * 0.9f);
    }
    circle(tx, ty, 20 * bloom + 8, 0xffe08aff, alpha);
    glow(tx, ty, 160 * bloom, 120, 0xffc8f0ff, alpha * 0.4f * bloom);
  }
}

/* ---- effects ------------------------------------------------------------------ */

static void burst(float x, float y, int count, uint32_t color, float speed, float life, float size,
                  uint8_t kind) {
  for (int i = 0; i < count; ++i) {
    particle* p = &particles[particle_next++ % MAX_PARTICLES];
    const float a = rnd();
    const float v = speed * (0.4f + 0.6f * rnd());
    p->x = x;
    p->y = y;
    p->vx = synth_cos(a) * v;
    p->vy = synth_sin(a) * v - (kind == 1 ? speed * 0.6f : 0);
    p->life = p->max = life * (0.6f + 0.4f * rnd());
    p->size = size * (0.6f + 0.6f * rnd());
    p->color = color;
    p->kind = kind;
  }
}

static void add_ring(float x, float y, float radius, float span, uint32_t color) {
  ring* r = &rings[ring_next++ % MAX_RINGS];
  r->x = x;
  r->y = y;
  r->t0 = (float)now_s;
  r->span = span;
  r->radius = radius;
  r->color = color;
}

static void effect(uint8_t kind, float x, float y, tsugumi_view_time at) {
  switch (kind) {
    case 0: burst(x, y, 10, 0xffe08aff, 160, 0.6f, 5, 0); synth_click(at); break;
    case 1: burst(x, y, 18, 0x9fe08aff, 140, 0.9f, 5, 1); add_ring(x, y, 60, 0.5f, 0x9fe08aff); synth_buy(at); break;
    case 2: burst(x, y, 50, 0xbff0ffff, 220, 1.6f, 6, 1); add_ring(x, y, 420, 1.6f, 0xbff0ffff); synth_chime(at); break;
    default: burst(x, y, 90, 0xffa8e0ff, 260, 2.4f, 8, 1); add_ring(x, y, 520, 2.2f, 0xffd77aff); synth_bloom(at); break;
  }
}

static void draw_effects(float dt) {
  for (uint32_t i = 0; i < MAX_PARTICLES; ++i) {
    particle* p = &particles[i];
    if (p->life <= 0) continue;
    p->life -= dt;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->vx *= 1 - 1.6f * dt;
    p->vy = p->vy * (1 - 1.6f * dt) + (p->kind == 1 ? -10.0f : 30.0f) * dt;
    const float f = p->life / p->max;
    if (f > 0) star(p->x, p->y, p->size * (0.5f + f), p->color, f);
  }
  for (uint32_t i = 0; i < MAX_RINGS; ++i) {
    ring* r = &rings[i];
    if (r->span <= 0) continue;
    const float f = ((float)now_s - r->t0) / r->span;
    if (f >= 1) {
      r->span = 0;
      continue;
    }
    ring_shape(r->x, r->y, 10 + r->radius * f, 3 + 6 * (1 - f), r->color, (1 - f) * 0.8f);
  }
}

/* ---- screens ---------------------------------------------------------------- */

typedef struct { const char* name; uint32_t top, bottom, mote; } backdrop;
static const backdrop kBackdrops[] = {
    {"title", 0x0b1a2aff, 0x1d3b2aff, 0xfff0b0ff},  {"grow", 0x0c1d18ff, 0x253a1cff, 0xdfffa0ff},
    {"skills", 0x120f2aff, 0x1c3040ff, 0xb8d8ffff}, {"replant", 0x1c140fff, 0x3a2b1dff, 0xffd0a0ff},
    {"ending", 0x2a1630ff, 0xc98a5cff, 0xffd0f0ff},
};

static const backdrop* current_backdrop(void) {
  for (uint32_t i = 0; i < sizeof kBackdrops / sizeof kBackdrops[0]; ++i)
    if (glow_str_eq(scene.screen, kBackdrops[i].name)) return &kBackdrops[i];
  return &kBackdrops[0];
}

static void draw_backdrop(void) {
  const backdrop* b = current_backdrop();
  gradient(b->top, b->bottom);
  const float t = (float)now_s;
  for (int i = 0; i < 46; ++i) {
    const float fi = (float)i;
    const float x = 1280 * (0.5f + 0.5f * synth_sin(fi * 0.731f)) + 20 * synth_sin(t * 0.13f + fi);
    float y = 760 - (float)((int)(t * (8 + (float)(i % 7) * 3) + fi * 97) % 800);
    const float a = 0.25f + 0.25f * synth_sin(t * 0.5f + fi * 1.3f);
    glow(x, y, 3 + (float)(i % 3), 8, b->mote, a);
  }
  if (glow_str_eq(scene.screen, "grow")) box(0, 520, 1280, 200, 0, 0x1a140eff, 0.85f);
}

static int button_hit(const glow_button* b, float x, float y) {
  return x >= b->x && x <= b->x + b->w && y >= b->y && y <= b->y + b->h;
}

static int node_hit(const glow_node* n, float x, float y) {
  const float dx = x - n->x, dy = y - n->y;
  return dx * dx + dy * dy <= 40 * 40;
}

static float press_bounce(uint32_t index) {
  const double since = now_s - pressed_at[index];
  if (since < 0 || since > 0.18) return 0;
  return (float)(1 - since / 0.18) * 4;
}

static void draw_buttons(float alpha) {
  for (uint32_t i = 0; i < scene.button_count; ++i) {
    const glow_button* b = &scene.buttons[i];
    if (b->style == 3) continue; /* the sprout: the plant is its picture */
    const int hot = (int)i == hover_button && b->enabled;
    const float a = alpha * (b->enabled ? 1.0f : 0.45f);
    const float inset = press_bounce(i);
    const float x = b->x + inset, y = b->y + inset, w = b->w - 2 * inset, h = b->h - 2 * inset;
    if (b->style == 1) {
      if (hot) glow(x + w / 2, y + h / 2, w * 0.55f, 30, 0xffd77aff, a * 0.35f);
      box(x, y, w, h, h / 2, hot ? 0xffe39aff : 0xffd77aff, a);
      text_str(b->label, x + w / 2, y + h / 2, 28, 0x1c2414ff, a, 1);
      continue;
    }
    box(x, y, w, h, 14, b->style == 2 ? 0x1b221bff : hot ? 0x2f4a30ff : 0x223524ff, a * 0.92f);
    if (b->detail.size) {
      text_str(b->label, x + 22, y + h * 0.36f, 24, 0xf4efe0ff, a, 0);
      text_str(b->detail, x + 22, y + h * 0.72f, 18, 0xb9c4b0ff, a, 0);
    } else {
      text_str(b->label, x + w / 2, y + h / 2, 24, b->style == 2 ? 0x7d887aff : 0xf4efe0ff, a, 1);
    }
  }
}

static void draw_tree(float alpha) {
  const float t = (float)now_s;
  for (uint32_t i = 0; i < scene.node_count; ++i) { /* edges first */
    const glow_node* n = &scene.nodes[i];
    const int owned = n->state == 3;
    if (n->parent >= 0 && (uint32_t)n->parent < scene.node_count) {
      const glow_node* p = &scene.nodes[n->parent];
      box(n->x - 3, n->y, 6, p->y - n->y, 3, owned ? n->color : 0x55606aff, alpha * (owned ? 0.8f : 0.5f));
    } else if (i + 1 == scene.node_count) { /* the Bloom joins the tops of the branches */
      for (uint32_t j = 0; j + 1 < scene.node_count; ++j) {
        if (j % 4 != 3) continue;
        const glow_node* top = &scene.nodes[j];
        const float mid = (top->y + n->y) / 2 + 30;
        box(top->x - 2, mid, 4, top->y - mid, 2, 0x55606aff, alpha * 0.5f);
        const float x0 = top->x < n->x ? top->x : n->x, x1 = top->x < n->x ? n->x : top->x;
        box(x0, mid - 2, x1 - x0, 4, 2, 0x55606aff, alpha * 0.5f);
      }
      box(n->x - 2, n->y, 4, 70, 2, 0x55606aff, alpha * 0.5f);
    }
  }
  for (uint32_t i = 0; i < scene.node_count; ++i) {
    const glow_node* n = &scene.nodes[i];
    const int hot = (int)i == hover_node;
    const float r = 32 - press_bounce(GLOW_MAX_BUTTONS + i);
    switch (n->state) {
      case 3:
        glow(n->x, n->y, r + 18, 30, n->color, alpha * 0.45f);
        circle(n->x, n->y, r, n->color, alpha);
        break;
      case 1:
        glow(n->x, n->y, r + 10 + 4 * synth_sin(t * 3), 24, n->color, alpha * 0.35f);
        circle(n->x, n->y, r, 0x22303aff, alpha);
        ring_shape(n->x, n->y, r, 3, n->color, alpha);
        break;
      case 2:
        circle(n->x, n->y, r, 0x22303aff, alpha * 0.8f);
        ring_shape(n->x, n->y, r, 2, n->color, alpha * 0.45f);
        break;
      default:
        circle(n->x, n->y, r, 0x1a1f26ff, alpha * 0.7f);
        ring_shape(n->x, n->y, r, 2, 0x55606aff, alpha * 0.6f);
        break;
    }
    if (hot) ring_shape(n->x, n->y, r + 6, 2, 0xffffffff, alpha * 0.7f);
    text_str(n->label, n->x, n->y + r + 18, 18, n->state ? 0xf4efe0ff : 0x8a948cff, alpha, 1);
  }
  if (hover_node >= 0 && (uint32_t)hover_node < scene.node_count) {
    const glow_node* n = &scene.nodes[hover_node];
    const float w = text_width(n->detail.data, n->detail.size, 18) + 32;
    float x = n->x - w / 2;
    if (x < 10) x = 10;
    if (x + w > 1270) x = 1270 - w;
    const float y = n->y - 82;
    box(x, y, w, 40, 10, 0x0d1218ff, alpha * 0.92f);
    text_str(n->detail, x + w / 2, y + 20, 18, 0xf4efe0ff, alpha, 1);
  }
}

static void draw_labels(float alpha, float dt) {
  for (uint32_t i = 0; i < scene.label_count; ++i) {
    const glow_label* l = &scene.labels[i];
    if (!l->counter) {
      text_str(l->text, l->x, l->y, l->size, l->color, alpha, l->align);
      continue;
    }
    const double target = l->value;
    double* shown = &counters[i];
    const double gap = target - *shown;
    const double step = glow_str_eq(scene.screen, "ending") ? 1.8 * dt : 12.0 * dt;
    *shown += gap * (step > 1 ? 1 : step);
    if (gap > -0.5 && gap < 0.5) *shown = target;
    counter_text(l, *shown, alpha);
  }
}

static void draw_scene(float dt) {
  if (!have_scene) {
    gradient(0x0b1a2aff, 0x1d3b2aff);
    return;
  }
  draw_backdrop();
  const float fade = (float)((now_s - screen_since_s) / 0.35);
  const float alpha = fade > 1 ? 1 : fade < 0 ? 0 : fade;
  const float k = 1 - (dt * 6 > 1 ? 1 : dt * 6);
  shown_plant = shown_plant * k + scene.plant * (1 - k);
  shown_bloom = shown_bloom * k + scene.bloom * (1 - k);

  int sprout = -1;
  for (uint32_t i = 0; i < scene.button_count; ++i)
    if (scene.buttons[i].style == 3) sprout = (int)i;
  if (sprout >= 0) {
    const glow_button* b = &scene.buttons[sprout];
    const float squash = press_bounce((uint32_t)sprout) * 0.01f;
    if (sprout == hover_button) glow(b->x + b->w / 2, b->y + b->h - 160, 150, 90, 0xfff0b0ff, 0.12f);
    plant(b->x + b->w / 2, 520, shown_plant * (1 - squash), shown_bloom, alpha);
  } else if (scene.plant > 0) {
    if (glow_str_eq(scene.screen, "ending")) plant(170, 640, shown_plant, shown_bloom, alpha);
    else plant(640, 712, shown_plant * 0.8f, shown_bloom, alpha * 0.5f);
  }
  draw_tree(alpha);
  draw_buttons(alpha);
  draw_labels(alpha, dt);
  draw_effects(dt);
}

/* ---- the view ABI ---------------------------------------------------------- */

static const char shader_source[] =
    "struct U { scale: vec4f, view: vec4f }"
    "@group(0) @binding(0) var<uniform> u: U;"
    "@group(0) @binding(1) var samp: sampler;"
    "@group(0) @binding(2) var atlas: texture_2d<f32>;"
    "struct I { @location(0) rect: vec4f, @location(1) color: vec4f,"
    " @location(2) uv: vec4f, @location(3) p: vec4f }"
    "struct V { @builtin(position) pos: vec4f, @location(0) local: vec2f,"
    " @location(1) size: vec2f, @location(2) color: vec4f, @location(3) uv: vec4f,"
    " @location(4) p: vec4f, @location(5) t: vec2f }"
    "@vertex fn vs(@builtin(vertex_index) vi: u32, i: I) -> V {"
    " let corners = array(vec2f(0, 0), vec2f(1, 0), vec2f(0, 1), vec2f(1, 0), vec2f(1, 1), vec2f(0, 1));"
    " let c = corners[vi];"
    " let pad = i.p.z;"
    " let size = i.rect.zw + vec2f(2 * pad);"
    " let world = i.rect.xy - vec2f(pad) + c * size;"
    " let px = u.scale.yz + world * u.scale.x;"
    " var o: V;"
    " o.pos = vec4f(px.x / u.view.x * 2 - 1, 1 - px.y / u.view.y * 2, 0, 1);"
    " o.local = c * size - vec2f(pad) - i.rect.zw * 0.5;"
    " o.size = i.rect.zw; o.color = i.color; o.uv = i.uv; o.p = i.p; o.t = c;"
    " return o;"
    "}"
    "@fragment fn fs(v: V) -> @location(0) vec4f {"
    " let mode = u32(v.p.y + 0.5);"
    " let glyph = textureSampleLevel(atlas, samp, mix(v.uv.xy, v.uv.zw, v.t), 0).r;"
    " if (mode == 1u) { return vec4f(v.color.rgb, 1) * v.color.a * glyph; }"
    " if (mode == 2u) { let c = mix(v.color, v.uv, v.t.y); return vec4f(c.rgb * c.a, c.a); }"
    " let half = max(v.size * 0.5, vec2f(0.001));"
    " var d: f32;"
    " if (mode == 4u) {"
    "  let q = abs(v.local) / half;"
    "  d = (sqrt(q.x) + sqrt(q.y) - 1) * min(half.x, half.y) * 0.5;"
    " } else {"
    "  let r = min(v.p.x, min(half.x, half.y));"
    "  let q = abs(v.local) - half + vec2f(r);"
    "  d = length(max(q, vec2f(0))) + min(max(q.x, q.y), 0) - r;"
    "  if (mode == 3u) { d = abs(d + v.p.w) - v.p.w; }"
    " }"
    " let aa = 0.9 / u.scale.x;"
    " var a = clamp(0.5 - d / aa, 0, 1);"
    " if (v.p.z > 0) { a = exp(-max(d, 0) * 3 / v.p.z) * (1 - 0.3 * clamp(-d / half.x, 0, 1)); }"
    " return vec4f(v.color.rgb, 1) * v.color.a * a;"
    "}";

int32_t tsugumi_view_abi(void) { return TSUGUMI_VIEW_ABI; }

int32_t tsugumi_view_resize(uint32_t w, uint32_t h, float device_scale, tsugumi_view_time at) {
  (void)at;
  width = w;
  height = h;
  scale = device_scale;
  return 0;
}

int32_t tsugumi_view_start(WGPUDevice gpu, WGPUQueue commands, WGPUTextureFormat format, uint32_t w,
                           uint32_t h, float device_scale) {
  device = gpu;
  queue = commands;
  tsugumi_view_resize(w, h, device_scale, 0);
  synth_start();
  font = tsugumi_dawn_font_get_metrics();
  WGPUTexture atlas = tsugumi_dawn_font_create_texture(device, queue);
  if (!atlas) return -1;
  WGPUTextureView atlas_view = wgpuTextureCreateView(atlas, NULL);

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = (WGPUStringView){shader_source, sizeof(shader_source) - 1};
  WGPUShaderModuleDescriptor shader_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  shader_desc.nextInChain = &wgsl.chain;
  WGPUShaderModule shader = wgpuDeviceCreateShaderModule(device, &shader_desc);
  if (!shader) return -1;

  WGPUVertexAttribute attributes[4];
  for (uint32_t i = 0; i < 4; ++i) {
    attributes[i] = (WGPUVertexAttribute)WGPU_VERTEX_ATTRIBUTE_INIT;
    attributes[i].format = WGPUVertexFormat_Float32x4;
    attributes[i].offset = 16 * i;
    attributes[i].shaderLocation = i;
  }
  WGPUVertexBufferLayout layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
  layout.stepMode = WGPUVertexStepMode_Instance;
  layout.arrayStride = sizeof(quad);
  layout.attributeCount = 4;
  layout.attributes = attributes;

  WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
  blend.color.srcFactor = WGPUBlendFactor_One;
  blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
  blend.color.operation = WGPUBlendOperation_Add;
  blend.alpha = blend.color;
  WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
  color.format = format;
  color.blend = &blend;
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = shader;
  fragment.entryPoint = (WGPUStringView){"fs", 2};
  fragment.targetCount = 1;
  fragment.targets = &color;
  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.vertex.module = shader;
  desc.vertex.entryPoint = (WGPUStringView){"vs", 2};
  desc.vertex.bufferCount = 1;
  desc.vertex.buffers = &layout;
  desc.fragment = &fragment;
  pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
  wgpuShaderModuleRelease(shader);
  if (!pipeline) return -1;

  WGPUBufferDescriptor quads_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  quads_desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
  quads_desc.size = sizeof quads;
  quads_buffer = wgpuDeviceCreateBuffer(device, &quads_desc);
  WGPUBufferDescriptor uniform_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  uniform_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  uniform_desc.size = 32;
  uniform_buffer = wgpuDeviceCreateBuffer(device, &uniform_desc);

  WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
  sampler_desc.magFilter = WGPUFilterMode_Linear;
  sampler_desc.minFilter = WGPUFilterMode_Linear;
  WGPUSampler sampler = wgpuDeviceCreateSampler(device, &sampler_desc);

  WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                   WGPU_BIND_GROUP_ENTRY_INIT};
  entries[0].binding = 0;
  entries[0].buffer = uniform_buffer;
  entries[0].size = 32;
  entries[1].binding = 1;
  entries[1].sampler = sampler;
  entries[2].binding = 2;
  entries[2].textureView = atlas_view;
  WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  group_desc.layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
  group_desc.entryCount = 3;
  group_desc.entries = entries;
  bind_group = wgpuDeviceCreateBindGroup(device, &group_desc);
  wgpuBindGroupLayoutRelease(group_desc.layout);
  return quads_buffer && uniform_buffer && bind_group ? 0 : -1;
}

int32_t tsugumi_view_message(uint32_t size, tsugumi_view_time at) {
  if (size > SCENE_BYTES) return 0; /* not a scene of this game: ignore it */
  uint8_t* bytes = scene_bytes[scene_slot ^ 1];
  if (tsugumi_view_read(bytes, size) != 0) return -1;
  static glow_scene next;
  if (glow_scene_read(bytes, size, &next) != 0) return 0;
  const int same_screen = have_scene && glow_str_eq(next.screen, screen_name);
  scene_slot ^= 1;
  scene = next;
  if (!same_screen) {
    uint32_t n = scene.screen.size < sizeof screen_name - 1 ? scene.screen.size : sizeof screen_name - 1;
    __builtin_memcpy(screen_name, scene.screen.data, n);
    screen_name[n] = 0;
    screen_since_s = now_s;
    synth_screen(screen_name, at);
    hover_button = hover_node = -1;
    for (uint32_t i = 0; i < GLOW_MAX_LABELS; ++i)
      counters[i] = glow_str_eq(scene.screen, "ending") || i >= scene.label_count ? 0 : scene.labels[i].value;
    if (!have_scene) {
      shown_plant = scene.plant;
      shown_bloom = scene.bloom;
    }
  }
  have_scene = 1;
  for (uint32_t i = 0; i < scene.fx_count; ++i) effect(scene.fx[i].kind, scene.fx[i].x, scene.fx[i].y, at);
  return 0;
}

static float fit(void) {
  const float sx = (float)width / kWidth, sy = (float)height / kHeight;
  return sx < sy ? sx : sy;
}

/* Logical pointer pixels to screen units. */
static void to_screen(float x, float y, float* sx, float* sy) {
  const float k = fit();
  const float ox = ((float)width - kWidth * k) / 2, oy = ((float)height - kHeight * k) / 2;
  *sx = (x * scale - ox) / k;
  *sy = (y * scale - oy) / k;
}

static void send_event(glow_str id, float x, float y) {
  uint8_t event[160];
  const uint32_t n = glow_event_write(event, sizeof event, id, x, y);
  if (n) tsugumi_view_send(scene.reply, event, n);
}

static void hover(float x, float y) {
  hover_button = hover_node = -1;
  for (uint32_t i = 0; i < scene.button_count; ++i)
    if (button_hit(&scene.buttons[i], x, y)) hover_button = (int)i;
  for (uint32_t i = 0; i < scene.node_count; ++i)
    if (node_hit(&scene.nodes[i], x, y)) hover_node = (int)i;
}

static void press(float x, float y, tsugumi_view_time at) {
  hover(x, y);
  if (hover_button >= 0) {
    const glow_button* b = &scene.buttons[hover_button];
    if (!b->enabled) {
      synth_thud(at);
      return;
    }
    pressed_at[hover_button] = now_s;
    if (b->style == 3) burst(x, y, 8, 0xffe08aff, 150, 0.6f, 5, 0);
    synth_click(at);
    send_event(b->id, x, y);
  } else if (hover_node >= 0) {
    const glow_node* n = &scene.nodes[hover_node];
    pressed_at[GLOW_MAX_BUTTONS + hover_node] = now_s;
    if (n->state == 1) {
      synth_click(at);
      send_event(n->id, x, y);
    } else {
      synth_thud(at);
    }
  }
}

int32_t tsugumi_view_pointer(uint32_t kind, float x, float y, uint32_t buttons, float wheel_x,
                             float wheel_y, uint32_t modifiers, tsugumi_view_time at) {
  (void)wheel_x;
  (void)wheel_y;
  (void)modifiers;
  if (!have_scene || !width || !height) return 0;
  float sx, sy;
  to_screen(x, y, &sx, &sy);
  pointer_x = sx;
  pointer_y = sy;
  if (kind == TSUGUMI_DAWN_POINTER_LEAVE) {
    hover_button = hover_node = -1;
    return 0;
  }
  if (kind == TSUGUMI_DAWN_POINTER_DOWN && (buttons & TSUGUMI_DAWN_BUTTON_PRIMARY)) press(sx, sy, at);
  else hover(sx, sy);
  return 0;
}

/* Space or Enter presses the sprout or the first primary button. */
int32_t tsugumi_view_key(uint32_t kind, uint32_t code, uint32_t modifiers, tsugumi_view_time at) {
  (void)modifiers;
  if (kind != TSUGUMI_DAWN_KEY_DOWN || !have_scene || (code != 0x2c && code != 0x28)) return 0;
  for (uint32_t i = 0; i < scene.button_count; ++i) {
    const glow_button* b = &scene.buttons[i];
    if (!b->enabled || (b->style != 3 && b->style != 1)) continue;
    if ((code == 0x2c) != (b->style == 3)) continue;
    pressed_at[i] = now_s;
    burst(b->x + b->w / 2, b->style == 3 ? 520 - 40 - 300 * shown_plant : b->y + b->h / 2, 8,
          0xffe08aff, 150, 0.6f, 5, 0);
    synth_click(at);
    send_event(b->id, b->x + b->w / 2, b->y + b->h / 2);
    return 0;
  }
  return 0;
}

const float* tsugumi_view_audio(uint32_t frames, tsugumi_view_time heard) {
  return synth_render(frames, heard);
}

int32_t tsugumi_view_draw(WGPUTextureView target, tsugumi_view_time present) {
  if (width == 0 || height == 0) return TSUGUMI_DAWN_DRAW_AGAIN;
  now_s = tsugumi_dawn_seconds(present);
  float dt = (float)(now_s - last_s);
  if (dt < 0 || dt > 0.1f) dt = 0.016f;
  last_s = now_s;

  quad_count = 0;
  draw_scene(dt);

  const float k = fit();
  const float uniforms[8] = {k, ((float)width - kWidth * k) / 2, ((float)height - kHeight * k) / 2, 0,
                             (float)width, (float)height, (float)now_s, 0};
  wgpuQueueWriteBuffer(queue, uniform_buffer, 0, uniforms, sizeof uniforms);
  if (quad_count) wgpuQueueWriteBuffer(queue, quads_buffer, 0, quads, quad_count * sizeof(quad));

  WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  attachment.view = target;
  attachment.loadOp = WGPULoadOp_Clear;
  attachment.storeOp = WGPUStoreOp_Store;
  attachment.clearValue = (WGPUColor){0.02, 0.03, 0.03, 1};
  WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &attachment;
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
  if (quad_count) {
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, quads_buffer, 0, quad_count * sizeof(quad));
    wgpuRenderPassEncoderDraw(pass, 6, quad_count, 0, 0);
  }
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, NULL);
  wgpuQueueSubmit(queue, 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);
  return TSUGUMI_DAWN_DRAW_AGAIN;
}
