#nullable enable
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Tsugumi;

// NativeValue, NativeSeries and NativeCall (Context.cs) mirror
// sdk/include/tsugumi/sdk/abi.h by hand. host.cc passes the C compiler's
// offsetof/sizeof of every field; this compares them with the managed layout
// by C field name, so a field missing, moved or resized on either side fails
// tsugumi_load. When abi.h gains a field, add it to host.cc's k_layout, to
// the managed struct and here. tests/static/test_csharp_abi.py also checks
// field order and callback signatures without building a Card.
internal static unsafe class Layout {
    [StructLayout(LayoutKind.Sequential)]
    private struct Field {
        internal byte* Name;
        internal ulong Offset;
        internal ulong Size;
    }

    internal static bool Check(nint fields, nuint count) {
        var native = new Dictionary<string, (ulong Offset, ulong Size)>();
        var table = (Field*)fields;
        for (nuint i = 0; i < count; i++)
            native[Marshal.PtrToStringUTF8((nint)table[i].Name) ?? ""] = (table[i].Offset, table[i].Size);
        var errors = new List<string>();
        foreach (var (name, offset, size) in Managed()) {
            if (!native.Remove(name, out var c)) errors.Add($"{name} has no field in abi.h");
            else if (c.Offset != offset || c.Size != size)
                errors.Add($"{name} is at {c.Offset} ({c.Size} bytes) in abi.h but at {offset} ({size} bytes) in C#");
        }
        foreach (var name in native.Keys) errors.Add($"{name} of abi.h has no C# field");
        foreach (var error in errors) Console.Error.WriteLine($"C# SDK layout: {error}");
        return errors.Count == 0;
    }

    private static List<(string Name, ulong Offset, ulong Size)> Managed() {
        var list = new List<(string, ulong, ulong)>();
        ulong pointer = (ulong)sizeof(nint);
        void Add(string name, void* start, void* field, ulong size) =>
            list.Add((name, (ulong)((byte*)field - (byte*)start), size));

        NativeValue value = default;
        var v = &value;
        list.Add(("tsugumi_value", 0, (ulong)sizeof(NativeValue)));
        Add("tsugumi_value.name", v, &v->Name, pointer);
        Add("tsugumi_value.kind", v, &v->Kind, sizeof(uint));
        Add("tsugumi_value.data", v, &v->Data, pointer);
        Add("tsugumi_value.size", v, &v->Size, (ulong)sizeof(nuint));

        NativeBlob blob = default;
        var b = &blob;
        list.Add(("tsugumi_blob", 0, (ulong)sizeof(NativeBlob)));
        Add("tsugumi_blob.name", b, &b->Name, pointer);
        Add("tsugumi_blob.data", b, &b->Data, pointer);
        Add("tsugumi_blob.size", b, &b->Size, (ulong)sizeof(nuint));

        NativeSeries series = default;
        var s = &series;
        list.Add(("tsugumi_series", 0, (ulong)sizeof(NativeSeries)));
        Add("tsugumi_series.id", s, &s->Id, pointer);
        Add("tsugumi_series.label", s, &s->Label, pointer);
        Add("tsugumi_series.x", s, &s->X, pointer);
        Add("tsugumi_series.y", s, &s->Y, pointer);
        Add("tsugumi_series.count", s, &s->Count, (ulong)sizeof(nuint));

        NativeCall call = default;
        var c = &call;
        list.Add(("tsugumi_call", 0, (ulong)sizeof(NativeCall)));
        Add("tsugumi_call.abi_version", c, &c->AbiVersion, sizeof(uint));
        Add("tsugumi_call.run_id", c, &c->RunId, pointer);
        Add("tsugumi_call.card", c, &c->Card, pointer);
        Add("tsugumi_call.invocation", c, &c->Invocation, sizeof(ulong));
        Add("tsugumi_call.inputs", c, &c->Inputs, pointer);
        Add("tsugumi_call.input_count", c, &c->InputCount, (ulong)sizeof(nuint));
        Add("tsugumi_call.params", c, &c->Params, pointer);
        Add("tsugumi_call.params_size", c, &c->ParamsSize, (ulong)sizeof(nuint));
        Add("tsugumi_call.working_directory", c, &c->Work, pointer);
        Add("tsugumi_call.temporary_directory", c, &c->Temp, pointer);
        Add("tsugumi_call.output_directory", c, &c->Output, pointer);
        Add("tsugumi_call.host", c, &c->Host, pointer);
        Add("tsugumi_call.emit_output", c, &c->Emit, pointer);
        Add("tsugumi_call.display_text", c, &c->Text, pointer);
        Add("tsugumi_call.display_scalar", c, &c->Scalar, pointer);
        Add("tsugumi_call.display_line_chart", c, &c->LineChart, pointer);
        Add("tsugumi_call.log", c, &c->Log, pointer);
        Add("tsugumi_call.cancelled", c, &c->Cancelled, pointer);
        Add("tsugumi_call.remaining_ms", c, &c->Remaining, pointer);
        Add("tsugumi_call.fail", c, &c->Fail, pointer);
        Add("tsugumi_call.display_table", c, &c->Table, pointer);
        Add("tsugumi_call.display_image", c, &c->Image, pointer);
        Add("tsugumi_call.display_points", c, &c->Points, pointer);
        Add("tsugumi_call.display_module", c, &c->Module, pointer);
        Add("tsugumi_call.display_markdown", c, &c->Markdown, pointer);
        Add("tsugumi_call.input_json", c, &c->InputJson, pointer);
        Add("tsugumi_call.params_json", c, &c->ParamsJson, pointer);
        Add("tsugumi_call.emit_json", c, &c->EmitJson, pointer);
        Add("tsugumi_call.display_json", c, &c->DisplayJson, pointer);
        return list;
    }
}
