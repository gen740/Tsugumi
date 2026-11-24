"""Check the shared model of the pinned WebGPU C ABI."""

from copy import deepcopy
import os
from pathlib import Path
import unittest

import yaml

from gen_webgpu import Model, js_enum, parse_imports, val_type, value_args


class GeneratorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = yaml.safe_load(Path(os.environ["TSUGUMI_WEBGPU_YML"]).read_text())
        cls.names = parse_imports(Path(os.environ["TSUGUMI_VIEW_HEADER"]).read_text())
        cls.model = Model(cls.schema, cls.names)

    def test_layout(self):
        structs = self.model.structs
        for name, size, alignment, offsets in [
            ("string_view", 8, 4, {"length": 4}),
            ("chained_struct", 8, 4, {"s_type": 4}),
            ("bind_group_entry", 40, 8, {"offset": 16, "size": 24, "texture_view": 36}),
            ("render_pipeline_descriptor", 96, 4, {"vertex": 16, "depth_stencil": 72, "fragment": 92}),
            ("vertex_attribute", 24, 8, {"offset": 8, "shader_location": 16}),
            ("sampler_descriptor", 52, 4, {"max_anisotropy": 48}),
        ]:
            with self.subTest(struct=name):
                struct = structs[name]
                self.assertEqual((struct.size, struct.alignment), (size, alignment))
                self.assertEqual({field: struct.fields[field].offset for field in offsets}, offsets)

    def test_closure(self):
        self.assertEqual(len(self.names), 49)
        self.assertEqual(self.names, [fn.name for fn in self.model.functions])
        for name in ("shader_source_WGSL", "depth_stencil_state", "vertex_attribute", "color",
                     "pass_timestamp_writes", "texel_copy_buffer_layout", "texel_copy_texture_info"):
            self.assertIn(name, self.model.structs)
        for name in ("shader_source_SPIRV", "surface_descriptor", "adapter_info",
                     "limits", "render_bundle_descriptor"):
            self.assertNotIn(name, self.model.structs)
        self.assertIn("query_set", self.model.kinds)

    def test_wasm_signature(self):
        functions = {fn.name: fn for fn in self.model.functions}
        args = list(value_args(functions["wgpuQueueWriteBuffer"]))
        self.assertEqual([val_type(type_name) for _, type_name in args], ["I32", "I64", "I32", "I32"])
        args = list(value_args(functions["wgpuQueueSubmit"]))
        self.assertEqual(args, [("command_count", "usize"), ("commands", "address")])
        self.assertTrue(functions["wgpuBufferRelease"].release)
        self.assertFalse(functions["wgpuBufferDestroy"].release)

    def test_enum_and_flag_values(self):
        self.assertEqual(self.model.enums["texture_format"]["BGRA8_unorm"], 0x1B)
        self.assertEqual(self.model.enums["vertex_format"]["float32x3"], 0x1E)
        self.assertNotIn(0, self.model.enums["vertex_format"].values())
        self.assertEqual(self.model.enums["optional_bool"], {"false": 0, "true": 1, "undefined": 2})
        self.assertEqual(self.model.enums["s_type"]["shader_source_WGSL"], 2)
        self.assertEqual(self.model.flags["color_write_mask"]["all"], 0xF)
        self.assertEqual(self.model.flags["buffer_usage"]["query_resolve"], 0x200)
        self.assertEqual(self.model.constant("strlen"), 0xFFFFFFFF)
        self.assertEqual(self.model.constant("whole_size"), 0xFFFFFFFFFFFFFFFF)

    def test_idl_names(self):
        for name, entries in self.model.enums.items():
            if name != "s_type":
                for entry in entries:
                    js_enum(name, entry)
        for type_name, c_name, js_name in [
            ("texture_format", "ASTC_12x10_unorm_srgb", '"astc-12x10-unorm-srgb"'),
            ("texture_format", "ETC2_RGB8A1_unorm", '"etc2-rgb8a1unorm"'),
            ("texture_format", "depth32_float_stencil8", '"depth32float-stencil8"'),
            ("texture_format", "RGBA8_unorm_srgb", '"rgba8unorm-srgb"'),
            ("vertex_format", "unorm10__10__10__2", '"unorm10-10-10-2"'),
            ("vertex_format", "unorm8x4_B_G_R_A", '"unorm8x4-bgra"'),
            ("buffer_binding_type", "binding_not_used", "NOT_USED"),
            ("optional_bool", "false", "false"),
        ]:
            self.assertEqual(js_enum(type_name, c_name), js_name)
        with self.assertRaisesRegex(ValueError, "missing WebGPU IDL spelling"):
            js_enum("texture_format", "RGBA42_unorm")

    def test_unsupported_imports(self):
        for name, error in [
            ("wgpuQueueOnSubmittedWorkDone", "callbacks"),
            ("wgpuDeviceRelease", "host owns"),
            ("wgpuDeviceGetLimits", "expected an owned object return"),
            ("wgpuNonexistent", "expected an object method"),
        ]:
            with self.subTest(import_name=name), self.assertRaisesRegex(ValueError, error):
                Model(self.schema, [name])

    def test_unsupported_memory_shapes(self):
        for edit, error in [
            ({"pointer": "mutable"}, "writable"),
            ({"pointer": "immutable", "type": "uint32"}, "unsupported pointer"),
            ({"pointer": None}, "by-value aggregate"),
            ({"pointer": None, "type": "array<uint32>"}, "unsupported array"),
            ({"pointer": "immutable", "type": "c_void_data_ptr", "name": "data"}, "unsupported pointer"),
        ]:
            schema = deepcopy(self.schema)
            device = next(obj for obj in schema["objects"] if obj["name"] == "device")
            method = next(method for method in device["methods"] if method["name"] == "create_buffer")
            method["args"][0].update(edit)
            with self.subTest(edit=edit), self.assertRaisesRegex(ValueError, error):
                Model(schema, ["wgpuDeviceCreateBuffer"])


if __name__ == "__main__":
    unittest.main()
