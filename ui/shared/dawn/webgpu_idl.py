"""IDL spellings used by the generator, from https://www.w3.org/TR/webgpu/."""


# Validate the mechanically translated names too, so a new upstream spelling
# must be reviewed against WebIDL before it can reach a browser.
IDL_ENUMS = {
    "address_mode": "clamp-to-edge repeat mirror-repeat",
    "blend_factor": (
        "zero one src one-minus-src src-alpha one-minus-src-alpha dst one-minus-dst dst-alpha "
        "one-minus-dst-alpha src-alpha-saturated constant one-minus-constant src1 one-minus-src1 "
        "src1-alpha one-minus-src1-alpha"
    ),
    "blend_operation": "add subtract reverse-subtract min max",
    "buffer_binding_type": "uniform storage read-only-storage",
    "compare_function": "never less equal less-equal greater not-equal greater-equal always",
    "cull_mode": "none front back",
    "filter_mode": "nearest linear",
    "front_face": "ccw cw",
    "index_format": "uint16 uint32",
    "load_op": "load clear",
    "mipmap_filter_mode": "nearest linear",
    "primitive_topology": "point-list line-list line-strip triangle-list triangle-strip",
    "sampler_binding_type": "filtering non-filtering comparison",
    "stencil_operation": (
        "keep zero replace invert increment-clamp decrement-clamp increment-wrap decrement-wrap"
    ),
    "storage_texture_access": "write-only read-only read-write",
    "store_op": "store discard",
    "texture_aspect": "all stencil-only depth-only",
    "texture_dimension": "1d 2d 3d",
    "texture_format": (
        "r8unorm r8snorm r8uint r8sint r16unorm r16snorm r16uint r16sint r16float rg8unorm rg8snorm "
        "rg8uint rg8sint r32uint r32sint r32float rg16unorm rg16snorm rg16uint rg16sint rg16float "
        "rgba8unorm rgba8unorm-srgb rgba8snorm rgba8uint rgba8sint bgra8unorm bgra8unorm-srgb "
        "rgb9e5ufloat rgb10a2uint rgb10a2unorm rg11b10ufloat rg32uint rg32sint rg32float rgba16unorm "
        "rgba16snorm rgba16uint rgba16sint rgba16float rgba32uint rgba32sint rgba32float stencil8 "
        "depth16unorm depth24plus depth24plus-stencil8 depth32float depth32float-stencil8 "
        "bc1-rgba-unorm bc1-rgba-unorm-srgb bc2-rgba-unorm bc2-rgba-unorm-srgb bc3-rgba-unorm "
        "bc3-rgba-unorm-srgb bc4-r-unorm bc4-r-snorm bc5-rg-unorm bc5-rg-snorm bc6h-rgb-ufloat "
        "bc6h-rgb-float bc7-rgba-unorm bc7-rgba-unorm-srgb etc2-rgb8unorm etc2-rgb8unorm-srgb "
        "etc2-rgb8a1unorm etc2-rgb8a1unorm-srgb etc2-rgba8unorm etc2-rgba8unorm-srgb eac-r11unorm "
        "eac-r11snorm eac-rg11unorm eac-rg11snorm astc-4x4-unorm astc-4x4-unorm-srgb astc-5x4-unorm "
        "astc-5x4-unorm-srgb astc-5x5-unorm astc-5x5-unorm-srgb astc-6x5-unorm astc-6x5-unorm-srgb "
        "astc-6x6-unorm astc-6x6-unorm-srgb astc-8x5-unorm astc-8x5-unorm-srgb astc-8x6-unorm "
        "astc-8x6-unorm-srgb astc-8x8-unorm astc-8x8-unorm-srgb astc-10x5-unorm astc-10x5-unorm-srgb "
        "astc-10x6-unorm astc-10x6-unorm-srgb astc-10x8-unorm astc-10x8-unorm-srgb astc-10x10-unorm "
        "astc-10x10-unorm-srgb astc-12x10-unorm astc-12x10-unorm-srgb astc-12x12-unorm "
        "astc-12x12-unorm-srgb"
    ),
    "texture_sample_type": "float unfilterable-float depth sint uint",
    "texture_view_dimension": "1d 2d 2d-array cube cube-array 3d",
    "vertex_format": (
        "uint8 uint8x2 uint8x4 sint8 sint8x2 sint8x4 unorm8 unorm8x2 unorm8x4 snorm8 snorm8x2 "
        "snorm8x4 uint16 uint16x2 uint16x4 sint16 sint16x2 sint16x4 unorm16 unorm16x2 unorm16x4 "
        "snorm16 snorm16x2 snorm16x4 float16 float16x2 float16x4 float32 float32x2 float32x3 "
        "float32x4 uint32 uint32x2 uint32x3 uint32x4 sint32 sint32x2 sint32x3 sint32x4 "
        "unorm10-10-10-2 unorm8x4-bgra snorm10-10-10-2"
    ),
    "vertex_step_mode": "vertex instance",
}

# Texture format separators are positional in IDL. Look up the spelling from
# the explicit table after removing the separators used by the C schema.
TEXTURE_FORMATS = {
    value.replace("-", ""): value for value in IDL_ENUMS["texture_format"].split()
}
VERTEX_FORMATS = {
    "unorm10__10__10__2": "unorm10-10-10-2",
    "unorm8x4_B_G_R_A": "unorm8x4-bgra",
    "snorm10__10__10__2": "snorm10-10-10-2",
}
