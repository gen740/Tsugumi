#include <tsugumi/tsugumi_view.h>

static WGPUDevice device;
static WGPUQueue queue;
static WGPURenderPipeline pipeline;
static uint32_t width, height;
static float scale = 1;
static uint32_t choice = 10;
static uint8_t ready;

static const char shader_source[] =
    "struct Vertex { @builtin(position) pos: vec4f,"
    " @location(0) @interpolate(flat) color: u32 }"
    "@vertex fn vertex(@builtin(vertex_index) index: u32,"
    " @builtin(instance_index) color: u32) -> Vertex {"
    " let p = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));"
    " return Vertex(vec4f(p[index], 0, 1), color);"
    "}"
    "@fragment fn fragment(v: Vertex) -> @location(0) vec4f {"
    " let colors = array(vec4f(0.20, 0.24, 0.32, 1),"
    " vec4f(0.40, 0.46, 0.58, 1), vec4f(0.14, 0.63, 0.71, 1),"
    " vec4f(0.42, 0.69, 0.48, 1));"
    " return colors[v.color];"
    "}";

int32_t tsugumi_view_abi(void) { return TSUGUMI_VIEW_ABI; }

int32_t tsugumi_view_resize(uint32_t w, uint32_t h, float device_scale) {
  width = w;
  height = h;
  scale = device_scale;
  return 0;
}

int32_t tsugumi_view_start(WGPUDevice gpu, WGPUQueue commands,
                           WGPUTextureFormat format, uint32_t w, uint32_t h,
                           float device_scale) {
  device = gpu;
  queue = commands;
  tsugumi_view_resize(w, h, device_scale);

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = (WGPUStringView){shader_source, sizeof(shader_source) - 1};
  WGPUShaderModuleDescriptor shader_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  shader_desc.nextInChain = &wgsl.chain;
  WGPUShaderModule shader = wgpuDeviceCreateShaderModule(device, &shader_desc);
  if (!shader) return -1;
  WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
  color.format = format;
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = shader;
  fragment.entryPoint = (WGPUStringView){"fragment", 8};
  fragment.targetCount = 1;
  fragment.targets = &color;
  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.vertex.module = shader;
  desc.vertex.entryPoint = (WGPUStringView){"vertex", 6};
  desc.fragment = &fragment;
  pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
  wgpuShaderModuleRelease(shader);
  return pipeline ? 0 : -1;
}

int32_t tsugumi_view_message(uint32_t size) {
  if (size != sizeof(ready)) return -1;
  return tsugumi_view_read(&ready, size);
}

int32_t tsugumi_view_pointer(uint32_t kind, float x, float y, uint32_t buttons,
                             float wheel_x, float wheel_y, uint32_t modifiers) {
  (void)wheel_x;
  (void)wheel_y;
  (void)modifiers;
  if (!ready || kind != 1 || !(buttons & 1) || width == 0 || height == 0)
    return 0;
  if (y * scale < height * 0.32f || y * scale > height * 0.68f) return 0;
  const float position = (x * scale / width - 0.10f) / 0.80f;
  const float fraction = position < 0 ? 0 : position > 1 ? 1 : position;
  const uint8_t value = (uint8_t)(fraction * 20 + 0.5f);
  if (tsugumi_view_send(&value, sizeof(value)) == 0) choice = value;
  return 0;
}

static void rectangle(WGPURenderPassEncoder pass, float x, float y, float w,
                      float h, uint32_t color) {
  wgpuRenderPassEncoderSetViewport(pass, x * width, y * height, w * width,
                                   h * height, 0, 1);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, color);
}

int32_t tsugumi_view_draw(WGPUTextureView target) {
  if (width == 0 || height == 0) return 0;
  WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  attachment.view = target;
  attachment.loadOp = WGPULoadOp_Clear;
  attachment.storeOp = WGPUStoreOp_Store;
  attachment.clearValue = (WGPUColor){0.08, 0.10, 0.14, 1};
  WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &attachment;
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  rectangle(pass, 0.10f, 0.47f, 0.80f, 0.06f, 0);
  for (uint32_t tick = 0; tick <= 20; ++tick)
    rectangle(pass, 0.098f + 0.04f * tick, 0.50f, 0.004f,
              tick % 5 == 0 ? 0.13f : 0.07f, 1);
  rectangle(pass, 0.088f + 0.04f * choice, 0.42f, 0.024f, 0.16f, ready ? 2 : 3);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, NULL);
  wgpuQueueSubmit(queue, 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);
  return 0;
}
