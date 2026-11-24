#include <stdbool.h>
#include <tsugumi/tsugumi_view.h>

enum {
  kChunkBytes = 1 << 20,
  kMaxPoints = 2000000,
  kPageBytes = 65536,
  kReset = 1,
};

typedef struct {
  float position[3];
  uint32_t rgba;
} Point;

typedef struct {
  uint32_t flags;
  uint32_t count;
  Point points[(kChunkBytes - 8) / sizeof(Point)];
} Chunk;

_Static_assert(sizeof(Point) == 16 && offsetof(Chunk, points) == 8,
               "Point cloud wire layout");
_Static_assert(sizeof(Chunk) <= TSUGUMI_MESSAGE_MAX, "Chunk fits the relay");

static Chunk message;
static uint32_t count;
static float min_bounds[3], max_bounds[3];
static bool receiving;
extern uint8_t __heap_base;

static WGPUDevice device;
static WGPUQueue queue;
static WGPURenderPipeline pipeline;
static WGPUBindGroup camera_group;
static WGPUBuffer camera_buffer, point_buffer;
static uint32_t point_capacity;
static WGPUTexture depth;
static WGPUTextureView depth_view;
static uint32_t width, height;
static float scale;

// Velocities are radians per 1/60 second, independent of display refresh.
#define kOrbitStep (TSUGUMI_VIEW_SAMPLE_RATE / 60u)
static float yaw = 0.45f, pitch = 0.55f, zoom = 1.0f;
static float yaw_velocity, pitch_velocity, last_x, last_y;
static tsugumi_view_time last_motion, last_present;
static bool dragging;

static const char shader_code[] =
    "struct Camera { center: vec4f, orbit: vec4f, viewport: vec4f };\n"
    "@group(0) @binding(0) var<uniform> camera: Camera;\n"
    "struct Vertex {\n"
    "  @builtin(position) position: vec4f,\n"
    "  @location(0) color: vec4f,\n"
    "  @location(1) corner: vec2f,\n"
    "};\n"
    "@vertex fn vertex(@builtin(vertex_index) index: u32,\n"
    "                  @location(0) point: vec3f,\n"
    "                  @location(1) color: vec4f) -> Vertex {\n"
    "  let corners = array(vec2f(-1,-1), vec2f(1,-1), vec2f(-1,1),\n"
    "                      vec2f(-1,1), vec2f(1,-1), vec2f(1,1));\n"
    "  let q = point - camera.center.xyz;\n"
    "  let cy = cos(camera.orbit.x); let sy = sin(camera.orbit.x);\n"
    "  let cp = cos(camera.orbit.y); let sp = sin(camera.orbit.y);\n"
    "  let horizontal = cy * q.x - sy * q.y;\n"
    "  let forward = sy * q.x + cy * q.y;\n"
    "  let vertical = cp * q.z - sp * forward;\n"
    "  let distance = camera.orbit.z - (sp * q.z + cp * forward);\n"
    "  let near = camera.orbit.w;\n"
    "  let far = camera.viewport.w;\n"
    "  let corner = corners[index];\n"
    "  let pixel = camera.viewport.z / camera.viewport.xy;\n"
    "  let aspect = camera.viewport.y / camera.viewport.x;\n"
    "  let clip = 1.8 * vec2f(aspect * horizontal, vertical);\n"
    "  let z = far * (distance - near) / (far - near);\n"
    "  return Vertex(vec4f(clip + corner * pixel * distance, z, distance), "
    "color, corner);\n"
    "}\n"
    "@fragment fn fragment(input: Vertex) -> @location(0) vec4f {\n"
    "  if (dot(input.corner, input.corner) > 1.0) { discard; }\n"
    "  return input.color;\n"
    "}\n";

static float clamp(float value, float low, float high) {
  return value < low ? low : value > high ? high : value;
}

static int create_pipeline(WGPUTextureFormat format) {
  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = (WGPUStringView){shader_code, sizeof(shader_code) - 1};
  WGPUShaderModuleDescriptor shader_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  shader_desc.nextInChain = &wgsl.chain;
  WGPUShaderModule shader = wgpuDeviceCreateShaderModule(device, &shader_desc);
  if (!shader) return -1;

  const WGPUVertexAttribute attributes[] = {
      {.format = WGPUVertexFormat_Float32x3, .shaderLocation = 0},
      {.format = WGPUVertexFormat_Unorm8x4, .offset = 12, .shaderLocation = 1},
  };
  WGPUVertexBufferLayout layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
  layout.stepMode = WGPUVertexStepMode_Instance;
  layout.arrayStride = sizeof(Point);
  layout.attributeCount = 2;
  layout.attributes = attributes;
  WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
  color.format = format;
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = shader;
  fragment.entryPoint = (WGPUStringView){"fragment", 8};
  fragment.targetCount = 1;
  fragment.targets = &color;
  WGPUDepthStencilState depth_state = WGPU_DEPTH_STENCIL_STATE_INIT;
  depth_state.format = WGPUTextureFormat_Depth24Plus;
  depth_state.depthWriteEnabled = WGPUOptionalBool_True;
  depth_state.depthCompare = WGPUCompareFunction_Less;
  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  desc.vertex.module = shader;
  desc.vertex.entryPoint = (WGPUStringView){"vertex", 6};
  desc.vertex.bufferCount = 1;
  desc.vertex.buffers = &layout;
  desc.fragment = &fragment;
  desc.depthStencil = &depth_state;
  pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
  wgpuShaderModuleRelease(shader);
  return pipeline ? 0 : -1;
}

static WGPUBuffer create_buffer(WGPUBufferUsage usage, uint64_t size) {
  WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  desc.usage = usage | WGPUBufferUsage_CopyDst;
  desc.size = size;
  return wgpuDeviceCreateBuffer(device, &desc);
}

static int create_camera(void) {
  camera_buffer = create_buffer(WGPUBufferUsage_Uniform, 12 * sizeof(float));
  if (!camera_buffer) return -1;
  WGPUBindGroupLayout layout =
      wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
  if (!layout) return -1;
  WGPUBindGroupEntry camera = WGPU_BIND_GROUP_ENTRY_INIT;
  camera.buffer = camera_buffer;
  camera.size = 12 * sizeof(float);
  WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  desc.layout = layout;
  desc.entryCount = 1;
  desc.entries = &camera;
  camera_group = wgpuDeviceCreateBindGroup(device, &desc);
  wgpuBindGroupLayoutRelease(layout);
  return camera_group ? 0 : -1;
}

int32_t tsugumi_view_abi(void) { return TSUGUMI_VIEW_ABI; }

int32_t tsugumi_view_start(WGPUDevice gpu, WGPUQueue commands,
                           WGPUTextureFormat format, uint32_t w, uint32_t h,
                           float device_scale) {
  device = gpu;
  queue = commands;
  if (create_pipeline(format) || create_camera() ||
      tsugumi_view_resize(w, h, device_scale, 0))
    return -1;
  return 0;
}

int32_t tsugumi_view_resize(uint32_t w, uint32_t h, float device_scale,
                            tsugumi_view_time at) {
  (void)at;
  scale = device_scale;
  if (w == width && h == height) return 0;
  width = w;
  height = h;
  if (depth_view) wgpuTextureViewRelease(depth_view);
  if (depth) {
    wgpuTextureDestroy(depth);
    wgpuTextureRelease(depth);
  }
  depth_view = NULL;
  depth = NULL;
  if (!width || !height) return 0;

  WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
  desc.usage = WGPUTextureUsage_RenderAttachment;
  desc.size = (WGPUExtent3D){width, height, 1};
  desc.format = WGPUTextureFormat_Depth24Plus;
  depth = wgpuDeviceCreateTexture(device, &desc);
  if (!depth) return -1;
  depth_view = wgpuTextureCreateView(depth, NULL);
  return depth_view ? 0 : -1;
}

static Point* reserve_points(uint32_t needed) {
  // Keep one contiguous allocation at the Wasm heap base. A reset reuses it.
  const uintptr_t base = ((uintptr_t)&__heap_base + 15u) & ~(uintptr_t)15u;
  const size_t pages =
      (base + needed * sizeof(Point) + kPageBytes - 1) / kPageBytes;
  const size_t available = __builtin_wasm_memory_size(0);
  if (pages > available &&
      __builtin_wasm_memory_grow(0, pages - available) == (size_t)-1)
    return NULL;
  return (Point*)base;
}

int32_t tsugumi_view_message(uint32_t size, tsugumi_view_time at) {
  (void)at;
  if (size < offsetof(Chunk, points) || size > sizeof(message) ||
      tsugumi_view_read((uint8_t*)&message, size))
    return -1;
  const uint32_t incoming = message.count;
  const uint32_t payload = size - offsetof(Chunk, points);
  if (payload % sizeof(Point) || incoming != payload / sizeof(Point) ||
      (message.flags & ~kReset))
    return -1;
  if (message.flags & kReset) {
    receiving = true;
    count = 0;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      min_bounds[axis] = INFINITY;
      max_bounds[axis] = -INFINITY;
    }
  }
  if (!receiving || incoming > kMaxPoints - count) return -1;
  if (!incoming) return 0;
  const uint32_t total = count + incoming;
  Point* points = reserve_points(total);
  if (!points) return -1;

  for (uint32_t i = 0; i < incoming; ++i) {
    const Point point = message.points[i];
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const float value = point.position[axis];
      if (!__builtin_isfinite(value)) return -1;
      if (value < min_bounds[axis]) min_bounds[axis] = value;
      if (value > max_bounds[axis]) max_bounds[axis] = value;
    }
    points[count + i] = point;
  }

  uint32_t first = count;
  if (total > point_capacity) {
    // Grow geometrically so a large cloud doesn't need a copy on every chunk.
    const uint32_t doubled = point_capacity * 2;
    const uint32_t capacity = doubled > total ? doubled : total;
    const uint32_t next = capacity > kMaxPoints ? kMaxPoints : capacity;
    WGPUBuffer buffer =
        create_buffer(WGPUBufferUsage_Vertex, next * sizeof(Point));
    if (!buffer) return -1;
    if (point_buffer) {
      wgpuBufferDestroy(point_buffer);
      wgpuBufferRelease(point_buffer);
    }
    point_buffer = buffer;
    point_capacity = next;
    first = 0;
  }
  wgpuQueueWriteBuffer(queue, point_buffer, first * sizeof(Point),
                       points + first, (total - first) * sizeof(Point));
  count = total;
  return 0;
}

static void orbit(float dyaw, float dpitch) {
  yaw += dyaw;
  if (yaw > 3.141593f) yaw -= 6.283185f;
  if (yaw < -3.141593f) yaw += 6.283185f;
  pitch = clamp(pitch + dpitch, -1.5f, 1.5f);
}

int32_t tsugumi_view_pointer(uint32_t kind, float x, float y, uint32_t buttons,
                             float wheel_x, float wheel_y, uint32_t modifiers,
                             tsugumi_view_time at) {
  (void)wheel_x;
  (void)modifiers;
  if (kind == 3) {
    zoom = clamp(zoom * (1.0f + clamp(wheel_y * 0.005f, -0.25f, 0.25f)), 0.28f,
                 8.0f);
  } else if (kind == 1 && (buttons & 1)) {
    dragging = true;
    yaw_velocity = pitch_velocity = 0.0f;
    last_motion = at;
    last_x = x;
    last_y = y;
  } else if (kind == 0 && dragging && (buttons & 1)) {
    const float dyaw = clamp((x - last_x) * 0.007f, -0.25f, 0.25f);
    const float dpitch = clamp((y - last_y) * 0.007f, -0.25f, 0.25f);
    const float steps = at > last_motion
                            ? (float)(at - last_motion) / kOrbitStep
                            : 1.0f;
    yaw_velocity = clamp(dyaw / steps, -0.25f, 0.25f);
    pitch_velocity = clamp(dpitch / steps, -0.25f, 0.25f);
    orbit(dyaw, dpitch);
    last_motion = at;
    last_x = x;
    last_y = y;
  } else if (kind == 2 || kind == 4 || (kind == 0 && !(buttons & 1))) {
    if (dragging) {
      if (at > last_motion && at - last_motion > TSUGUMI_VIEW_SAMPLE_RATE / 10u)
        yaw_velocity = pitch_velocity = 0.0f;
      last_present = at;
    }
    dragging = false;
  }
  return 0;
}

static void upload_camera(void) {
  float camera[12] = {0};
  float extent_squared = 0.0f;
  for (uint32_t axis = 0; axis < 3; ++axis) {
    const float radius = (max_bounds[axis] - min_bounds[axis]) * 0.5f;
    camera[axis] = min_bounds[axis] + radius;
    extent_squared += radius * radius;
  }
  const float extent = __builtin_sqrtf(extent_squared);
  const float radius = extent > 0.01f ? extent : 0.01f;
  // Back out further for a narrow viewport to keep the initial cloud in frame.
  const float aspect = (float)width / (float)height;
  const float distance =
      radius * 2.35f * zoom / (aspect < 1.0f ? aspect : 1.0f);
  camera[4] = yaw;
  camera[5] = pitch;
  camera[6] = distance;
  camera[7] = radius * 0.025f;
  camera[8] = (float)width;
  camera[9] = (float)height;
  camera[10] = 1.65f * scale;
  camera[11] = distance + radius * 2.0f;
  wgpuQueueWriteBuffer(queue, camera_buffer, 0, camera, sizeof(camera));
}

int32_t tsugumi_view_draw(WGPUTextureView target, tsugumi_view_time present) {
  if (!depth_view) return 0;
  if (!dragging && present > last_present &&
      (yaw_velocity != 0.0f || pitch_velocity != 0.0f)) {
    const float damping = 0.88f;
    const float steps = clamp((float)(present - last_present) / kOrbitStep, 0, 60);
    const uint32_t whole_steps = (uint32_t)steps;
    float retained = 1.0f;
    for (uint32_t i = 0; i < whole_steps; ++i) retained *= damping;
    retained *= 1.0f - (1.0f - damping) * (steps - whole_steps);

    const float distance = (1.0f - retained) / (1.0f - damping);
    orbit(yaw_velocity * distance, pitch_velocity * distance);
    if (pitch <= -1.5f || pitch >= 1.5f) pitch_velocity = 0.0f;
    yaw_velocity *= retained;
    pitch_velocity *= retained;
    if (__builtin_fabsf(yaw_velocity) < 0.001f) yaw_velocity = 0.0f;
    if (__builtin_fabsf(pitch_velocity) < 0.001f) pitch_velocity = 0.0f;
  }

  if (present > last_present) last_present = present;

  WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  color.view = target;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = (WGPUColor){0.016, 0.022, 0.045, 1.0};
  WGPURenderPassDepthStencilAttachment depth_attachment =
      WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
  depth_attachment.view = depth_view;
  depth_attachment.depthLoadOp = WGPULoadOp_Clear;
  depth_attachment.depthStoreOp = WGPUStoreOp_Discard;
  depth_attachment.depthClearValue = 1.0f;
  WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  desc.colorAttachments = &color;
  desc.colorAttachmentCount = 1;
  desc.depthStencilAttachment = &depth_attachment;
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, NULL);
  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(encoder, &desc);
  if (count) {
    upload_camera();
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, camera_group, 0, NULL);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, point_buffer, 0,
                                         count * sizeof(Point));
    wgpuRenderPassEncoderDraw(pass, 6, count, 0, 0);
  }
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, NULL);
  wgpuQueueSubmit(queue, 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);
  return !dragging && (yaw_velocity != 0.0f || pitch_velocity != 0.0f);
}
