// Line chart rendering for Display values with Dawn (WebGPU) on Metal.
//
// The chart is a full snapshot: set_line_chart replaces the data and marks the
// renderer dirty; render() uploads and draws at most once per call, so many
// updates between two frames collapse into one upload.
#include "tsugumi/renderer/renderer.h"

#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" bool tsg_is_metal_layer(void* layer);

namespace {

constexpr const char* kShader = R"(
struct VOut {
  @builtin(position) pos : vec4f,
  @location(0) color : vec4f,
};
@vertex fn vs(@location(0) p : vec2f, @location(1) c : vec4f) -> VOut {
  var o : VOut;
  o.pos = vec4f(p, 0.0, 1.0);
  o.color = c;
  return o;
}
@fragment fn fs(i : VOut) -> @location(0) vec4f {
  return i.color;
}
)";

struct Vertex {
  float x, y;
  float r, g, b, a;
};

struct Series {
  std::vector<double> x, y;
  float r, g, b, a;
};

}  // namespace

struct tsg_renderer {
  wgpu::Instance instance;
  wgpu::Surface surface;
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::RenderPipeline pipeline;
  wgpu::TextureFormat format = wgpu::TextureFormat::BGRA8Unorm;
  wgpu::Buffer vertices;
  uint64_t vertex_capacity = 0;
  uint32_t vertex_count = 0;
  uint32_t width = 0, height = 0;
  float scale = 1;
  bool configured = false;
  bool dirty = true;
  bool upload = true;
  float bg[3] = {0.06f, 0.07f, 0.09f};
  std::vector<Series> series;
  tsg_renderer_stats stats{};
};

namespace {

void set_error(char* error, size_t len, const std::string& msg) {
  if (error && len) std::snprintf(error, len, "%s", msg.c_str());
}

void configure(tsg_renderer* r) {
  if (r->width == 0 || r->height == 0) return;
  wgpu::SurfaceConfiguration c{};
  c.device = r->device;
  c.format = r->format;
  c.usage = wgpu::TextureUsage::RenderAttachment;
  c.width = r->width;
  c.height = r->height;
  c.presentMode = wgpu::PresentMode::Fifo;
  c.alphaMode = wgpu::CompositeAlphaMode::Auto;
  r->surface.Configure(&c);
  r->configured = true;
}

// Appends a segment as two triangles of `px` pixel width.
void segment(std::vector<Vertex>& out, float x0, float y0, float x1, float y1, float px, const tsg_renderer* r,
             const float* c) {
  float dx = (x1 - x0) * r->width, dy = (y1 - y0) * r->height;
  float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-6f) return;
  // Normal in pixels, converted back to NDC per axis.
  float nx = -dy / len * px / r->width, ny = dx / len * px / r->height;
  Vertex a{x0 + nx, y0 + ny, c[0], c[1], c[2], c[3]}, b{x0 - nx, y0 - ny, c[0], c[1], c[2], c[3]};
  Vertex d{x1 + nx, y1 + ny, c[0], c[1], c[2], c[3]}, e{x1 - nx, y1 - ny, c[0], c[1], c[2], c[3]};
  out.insert(out.end(), {a, b, d, d, b, e});
}

std::vector<Vertex> build(const tsg_renderer* r) {
  std::vector<Vertex> v;
  if (r->width == 0 || r->height == 0) return v;
  double xmin = INFINITY, xmax = -INFINITY, ymin = INFINITY, ymax = -INFINITY;
  for (const auto& s : r->series)
    for (size_t i = 0; i < s.x.size(); ++i) {
      xmin = std::min(xmin, s.x[i]);
      xmax = std::max(xmax, s.x[i]);
      ymin = std::min(ymin, s.y[i]);
      ymax = std::max(ymax, s.y[i]);
    }
  // Plot area in NDC with a margin of 12 points.
  float mx = 12.0f * r->scale * 2.0f / r->width, my = 12.0f * r->scale * 2.0f / r->height;
  float left = -1 + mx, right = 1 - mx, bottom = -1 + my, top = 1 - my;
  const float grid[4] = {0.35f, 0.38f, 0.45f, 1.0f};
  float hair = std::max(1.0f, r->scale) * 0.5f;
  segment(v, left, bottom, right, bottom, hair, r, grid);
  segment(v, left, bottom, left, top, hair, r, grid);
  for (int k = 1; k < 4; ++k) {
    float y = bottom + (top - bottom) * k / 4.0f;
    const float faint[4] = {0.25f, 0.27f, 0.32f, 1.0f};
    segment(v, left, y, right, y, hair * 0.5f, r, faint);
  }
  if (!(xmax > xmin)) { xmin -= 1; xmax += 1; }
  if (!(ymax > ymin)) { ymin -= 1; ymax += 1; }
  double pad = (ymax - ymin) * 0.05;
  ymin -= pad;
  ymax += pad;
  auto px = [&](double x) { return static_cast<float>(left + (x - xmin) / (xmax - xmin) * (right - left)); };
  auto py = [&](double y) { return static_cast<float>(bottom + (y - ymin) / (ymax - ymin) * (top - bottom)); };
  float width = 1.25f * r->scale;
  for (const auto& s : r->series) {
    const float c[4] = {s.r, s.g, s.b, s.a};
    for (size_t i = 1; i < s.x.size(); ++i) segment(v, px(s.x[i - 1]), py(s.y[i - 1]), px(s.x[i]), py(s.y[i]), width, r, c);
  }
  return v;
}

}  // namespace

extern "C" {

tsg_renderer* tsg_renderer_create(void* metal_layer, char* error, size_t error_len) {
  if (!tsg_is_metal_layer(metal_layer)) {
    set_error(error, error_len, "not a CAMetalLayer");
    return nullptr;
  }
  auto* r = new tsg_renderer();
  static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor idesc{};
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = &kTimedWaitAny;
  r->instance = wgpu::CreateInstance(&idesc);
  if (!r->instance) {
    set_error(error, error_len, "cannot create the WebGPU instance");
    delete r;
    return nullptr;
  }
  wgpu::SurfaceSourceMetalLayer src{};
  src.layer = metal_layer;
  wgpu::SurfaceDescriptor sdesc{};
  sdesc.nextInChain = &src;
  r->surface = r->instance.CreateSurface(&sdesc);
  wgpu::RequestAdapterOptions ao{};
  ao.compatibleSurface = r->surface;
  ao.backendType = wgpu::BackendType::Metal;
  std::string failure;
  r->instance.WaitAny(
      r->instance.RequestAdapter(&ao, wgpu::CallbackMode::WaitAnyOnly,
                                 [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView msg) {
                                   if (status == wgpu::RequestAdapterStatus::Success) r->adapter = std::move(a);
                                   else failure = std::string(msg.data, msg.length);
                                 }),
      UINT64_MAX);
  if (!r->adapter) {
    set_error(error, error_len, "no Metal adapter: " + failure);
    delete r;
    return nullptr;
  }
  wgpu::DeviceDescriptor dd{};
  dd.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
    std::fprintf(stderr, "tsugumi renderer: WebGPU error %d: %.*s\n", static_cast<int>(type),
                 static_cast<int>(msg.length), msg.data);
  });
  r->instance.WaitAny(
      r->adapter.RequestDevice(&dd, wgpu::CallbackMode::WaitAnyOnly,
                               [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView msg) {
                                 if (status == wgpu::RequestDeviceStatus::Success) r->device = std::move(d);
                                 else failure = std::string(msg.data, msg.length);
                               }),
      UINT64_MAX);
  if (!r->device) {
    set_error(error, error_len, "cannot create the device: " + failure);
    delete r;
    return nullptr;
  }
  r->queue = r->device.GetQueue();
  wgpu::SurfaceCapabilities caps{};
  r->surface.GetCapabilities(r->adapter, &caps);
  if (caps.formatCount > 0) r->format = caps.formats[0];

  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = kShader;
  wgpu::ShaderModuleDescriptor smd{};
  smd.nextInChain = &wgsl;
  wgpu::ShaderModule module = r->device.CreateShaderModule(&smd);
  wgpu::VertexAttribute attrs[2] = {};
  attrs[0].format = wgpu::VertexFormat::Float32x2;
  attrs[0].offset = 0;
  attrs[0].shaderLocation = 0;
  attrs[1].format = wgpu::VertexFormat::Float32x4;
  attrs[1].offset = 8;
  attrs[1].shaderLocation = 1;
  wgpu::VertexBufferLayout layout{};
  layout.arrayStride = sizeof(Vertex);
  layout.attributeCount = 2;
  layout.attributes = attrs;
  wgpu::ColorTargetState target{};
  target.format = r->format;
  wgpu::FragmentState fs{};
  fs.module = module;
  fs.entryPoint = "fs";
  fs.targetCount = 1;
  fs.targets = &target;
  wgpu::RenderPipelineDescriptor pd{};
  pd.vertex.module = module;
  pd.vertex.entryPoint = "vs";
  pd.vertex.bufferCount = 1;
  pd.vertex.buffers = &layout;
  pd.fragment = &fs;
  pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  r->pipeline = r->device.CreateRenderPipeline(&pd);
  return r;
}

void tsg_renderer_destroy(tsg_renderer* r) {
  if (!r) return;
  if (r->vertices) r->vertices.Destroy();
  if (r->configured) r->surface.Unconfigure();
  delete r;
}

void tsg_renderer_resize(tsg_renderer* r, uint32_t width, uint32_t height, float scale) {
  if (!r || (width == r->width && height == r->height && scale == r->scale)) return;
  r->width = width;
  r->height = height;
  r->scale = scale > 0 ? scale : 1;
  r->stats.width = width;
  r->stats.height = height;
  configure(r);
  r->dirty = r->upload = true;
}

void tsg_renderer_set_line_chart(tsg_renderer* r, const tsg_series* series, size_t count) {
  if (!r) return;
  r->series.clear();
  for (size_t i = 0; i < count; ++i) {
    Series s;
    s.x.assign(series[i].x, series[i].x + series[i].count);
    s.y.assign(series[i].y, series[i].y + series[i].count);
    s.r = series[i].r;
    s.g = series[i].g;
    s.b = series[i].b;
    s.a = series[i].a;
    r->series.push_back(std::move(s));
  }
  ++r->stats.updates;
  r->dirty = r->upload = true;
}

void tsg_renderer_set_background(tsg_renderer* r, float red, float green, float blue) {
  if (!r) return;
  r->bg[0] = red;
  r->bg[1] = green;
  r->bg[2] = blue;
  r->dirty = true;
}

int tsg_renderer_render(tsg_renderer* r) {
  if (!r || !r->dirty || !r->configured) return 0;
  if (r->upload) {
    auto verts = build(r);
    uint64_t bytes = verts.size() * sizeof(Vertex);
    if (bytes > r->vertex_capacity) {
      // Grow geometrically; the old buffer is released immediately.
      if (r->vertices) r->vertices.Destroy();
      uint64_t cap = std::max<uint64_t>(bytes, std::max<uint64_t>(r->vertex_capacity * 2, 64 * 1024));
      cap = (cap + 3) & ~uint64_t(3);
      wgpu::BufferDescriptor bd{};
      bd.size = cap;
      bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
      r->vertices = r->device.CreateBuffer(&bd);
      r->vertex_capacity = cap;
      r->stats.vertex_bytes = cap;
    }
    if (bytes) r->queue.WriteBuffer(r->vertices, 0, verts.data(), (bytes + 3) & ~uint64_t(3));
    r->vertex_count = static_cast<uint32_t>(verts.size());
    r->upload = false;
  }
  wgpu::SurfaceTexture st{};
  r->surface.GetCurrentTexture(&st);
  if (st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
      st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
    configure(r);
    return 0;
  }
  wgpu::TextureView view = st.texture.CreateView();
  wgpu::RenderPassColorAttachment att{};
  att.view = view;
  att.loadOp = wgpu::LoadOp::Clear;
  att.storeOp = wgpu::StoreOp::Store;
  att.clearValue = {r->bg[0], r->bg[1], r->bg[2], 1.0};
  wgpu::RenderPassDescriptor rp{};
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &att;
  wgpu::CommandEncoder enc = r->device.CreateCommandEncoder();
  wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
  if (r->vertex_count) {
    pass.SetPipeline(r->pipeline);
    pass.SetVertexBuffer(0, r->vertices, 0, uint64_t(r->vertex_count) * sizeof(Vertex));
    pass.Draw(r->vertex_count);
  }
  pass.End();
  wgpu::CommandBuffer cmd = enc.Finish();
  r->queue.Submit(1, &cmd);
  r->surface.Present();
  r->dirty = false;
  ++r->stats.frames;
  return 1;
}

void tsg_renderer_stats_get(const tsg_renderer* r, tsg_renderer_stats* out) {
  if (r && out) *out = r->stats;
}

}  // extern "C"
