// The Card knows only webgpu.h and Tsugumi's portable input/event boundary.
#include <math.h>
#include <stdio.h>
#include <tsugumi/view.h>

static WGPUDevice device;
static WGPUSurface surface;
static WGPUQueue queue;
static WGPURenderPipeline pipeline;
static WGPUBuffer uniform, points;
static WGPUBindGroup bind;
static unsigned count;
static float yaw = 0.6f, pitch = 0.3f, zoom = 1.0f, last_x, last_y;
static int dragging;
static double last_time;

static const char shader[] =
    "struct Camera { angles: vec2f, aspect: f32, zoom: f32 }\n"
    "@group(0) @binding(0) var<uniform> camera: Camera;\n"
    "struct Vertex { @builtin(position) position: vec4f, @location(0) color: vec3f }\n"
    "@vertex fn vs(@location(0) p: vec3f) -> Vertex {\n"
    "  let c = cos(camera.angles); let s = sin(camera.angles);\n"
    "  let q = vec3f(c.x*p.x+s.x*p.z, p.y, -s.x*p.x+c.x*p.z);\n"
    "  let r = vec3f(q.x, c.y*q.y-s.y*q.z, s.y*q.y+c.y*q.z);\n"
    "  var out: Vertex;\n"
    "  out.position = vec4f(r.x/camera.aspect*camera.zoom, r.y*camera.zoom, r.z*0.2+0.5, 1.0);\n"
    "  out.color = p*0.4+vec3f(0.5); return out;\n"
    "}\n"
    "@fragment fn fs(in: Vertex) -> @location(0) vec4f { return vec4f(in.color, 1.0); }\n";

static void draw(void) {
  if (!points) return;
  TsugumiViewSize size;
  tsugumi_view_size(&size);
  float camera[] = {yaw, pitch, size.width / fmaxf(1, size.height), zoom};
  wgpuQueueWriteBuffer(queue, uniform, 0, camera, sizeof(camera));
  WGPUSurfaceTexture texture = WGPU_SURFACE_TEXTURE_INIT;
  wgpuSurfaceGetCurrentTexture(surface, &texture);
  if (!texture.texture) {
    tsugumi_view_error("Could not acquire a WebGPU surface texture");
    return;
  }
  WGPUTextureView target = wgpuTextureCreateView(texture.texture, NULL);
  WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  color.view = target;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = (WGPUColor){0.03, 0.04, 0.07, 1};
  WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color;
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  wgpuRenderPassEncoderSetBindGroup(pass, 0, bind, 0, NULL);
  wgpuRenderPassEncoderSetVertexBuffer(pass, 0, points, 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDraw(pass, count, 1, 0, 0);
  wgpuRenderPassEncoderEnd(pass);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, NULL);
  wgpuQueueSubmit(queue, 1, &commands);
  // Presentation belongs to the host after the callback returns. In a browser,
  // Emdawnwebgpu does not implement the blocking wgpuSurfacePresent call.
  wgpuCommandBufferRelease(commands);
  wgpuRenderPassEncoderRelease(pass);
  wgpuCommandEncoderRelease(encoder);
  wgpuTextureViewRelease(target);
  wgpuTextureRelease(texture.texture);
}

static void pointer(TsugumiPointerKind kind, float x, float y, uint32_t buttons) {
  if (kind == TSUGUMI_POINTER_DOWN) dragging = (buttons & 1) != 0;
  if (kind == TSUGUMI_POINTER_MOVE && dragging) {
    yaw += (x - last_x) * 0.01f;
    pitch += (y - last_y) * 0.01f;
  }
  if (kind == TSUGUMI_POINTER_UP || kind == TSUGUMI_POINTER_CANCEL) dragging = 0;
  last_x = x;
  last_y = y;
}

static void wheel(float dx, float dy) {
  (void)dx;
  zoom = fmaxf(0.2f, fminf(4, zoom * expf(-dy * 0.001f)));
}

static void frame(double time) {
  if (last_time && !dragging) yaw += (float)fmin(time - last_time, 50) * 0.00015f;
  last_time = time;
  draw();
}

void tsugumi_view_initialise(WGPUDevice gpu, WGPUSurface output, WGPUTextureFormat format) {
  device = gpu;
  surface = output;
  queue = wgpuDeviceGetQueue(device);
  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = (WGPUStringView){shader, sizeof(shader) - 1};
  WGPUShaderModuleDescriptor shader_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  shader_desc.nextInChain = &wgsl.chain;
  WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &shader_desc);
  WGPUVertexAttribute attribute = WGPU_VERTEX_ATTRIBUTE_INIT;
  attribute.format = WGPUVertexFormat_Float32x3;
  WGPUVertexBufferLayout vertices = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
  vertices.arrayStride = 12;
  vertices.attributeCount = 1;
  vertices.attributes = &attribute;
  WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
  target.format = format;
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = module;
  fragment.entryPoint = (WGPUStringView){"fs", 2};
  fragment.targetCount = 1;
  fragment.targets = &target;
  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.vertex.module = module;
  desc.vertex.entryPoint = (WGPUStringView){"vs", 2};
  desc.vertex.bufferCount = 1;
  desc.vertex.buffers = &vertices;
  desc.fragment = &fragment;
  desc.primitive.topology = WGPUPrimitiveTopology_PointList;
  pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
  wgpuShaderModuleRelease(module);
  WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  buffer_desc.size = 16;
  buffer_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  uniform = wgpuDeviceCreateBuffer(device, &buffer_desc);
  WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
  WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
  entry.buffer = uniform;
  entry.size = 16;
  WGPUBindGroupDescriptor bind_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  bind_desc.layout = layout;
  bind_desc.entryCount = 1;
  bind_desc.entries = &entry;
  bind = wgpuDeviceCreateBindGroup(device, &bind_desc);
  wgpuBindGroupLayoutRelease(layout);
  tsugumi_view_on_pointer(pointer);
  tsugumi_view_on_wheel(wheel);
  tsugumi_view_on_frame(frame);
}

void tsugumi_view_update(void) {
  size_t config_size, bytes;
  const char* config = tsugumi_view_config(&config_size);
  const void* data = tsugumi_view_blob("points", &bytes);
  // This fixture's configuration has exactly one key, emitted by JSON.stringify.
  if (!config_size || sscanf(config, "{\"count\":%u}", &count) != 1 || !data ||
      count > 8 * 1024 * 1024 / 12 || bytes != (size_t)count * 12) {
    tsugumi_view_error("Expected float32 XYZ triples and a count");
    return;
  }
  if (points) { wgpuBufferDestroy(points); wgpuBufferRelease(points); }
  WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  desc.size = bytes ? bytes : 4;
  desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
  points = wgpuDeviceCreateBuffer(device, &desc);
  if (bytes) wgpuQueueWriteBuffer(queue, points, 0, data, bytes);
}

void tsugumi_view_resize(void) { last_time = 0; }

void tsugumi_view_dispose(void) {
  tsugumi_view_on_frame(NULL);
  tsugumi_view_on_pointer(NULL);
  tsugumi_view_on_wheel(NULL);
  if (points) { wgpuBufferDestroy(points); wgpuBufferRelease(points); points = NULL; }
  if (uniform) { wgpuBufferDestroy(uniform); wgpuBufferRelease(uniform); uniform = NULL; }
  if (bind) { wgpuBindGroupRelease(bind); bind = NULL; }
  if (pipeline) { wgpuRenderPipelineRelease(pipeline); pipeline = NULL; }
  if (queue) { wgpuQueueRelease(queue); queue = NULL; }
}
