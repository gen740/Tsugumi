#nullable enable
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Tsugumi;

// The native structs (InvocationScope.cs) mirror
// sdk/include/tsugumi/sdk/abi.h by hand. host.cc passes the C compiler's
// offsetof/sizeof of every field; this compares them with the managed layout
// by C field name, so a field missing, moved or resized on either side fails
// the load phase. When abi.h gains a field, add it to host.cc's k_layout, to
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

        NativeBlob bValue = default;
        var b = &bValue;
        list.Add(("tsugumi_blob", 0, (ulong)sizeof(NativeBlob)));
        Add("tsugumi_blob.name", b, &b->Name, pointer);
        Add("tsugumi_blob.data", b, &b->Data, pointer);
        Add("tsugumi_blob.size", b, &b->Size, (ulong)sizeof(nuint));

        NativeSeries sValue = default;
        var s = &sValue;
        list.Add(("tsugumi_series", 0, (ulong)sizeof(NativeSeries)));
        Add("tsugumi_series.id", s, &s->Id, pointer);
        Add("tsugumi_series.label", s, &s->Label, pointer);
        Add("tsugumi_series.x", s, &s->X, pointer);
        Add("tsugumi_series.y", s, &s->Y, pointer);
        Add("tsugumi_series.count", s, &s->Count, (ulong)sizeof(nuint));

        NativeScalar scalarValue = default;
        var scalar = &scalarValue;
        list.Add(("tsugumi_scalar", 0, (ulong)sizeof(NativeScalar)));
        Add("tsugumi_scalar.value", scalar, &scalar->Value, (ulong)sizeof(double));
        Add("tsugumi_scalar.label", scalar, &scalar->Label, pointer);

        NativeChart chartValue = default;
        var chart = &chartValue;
        list.Add(("tsugumi_chart", 0, (ulong)sizeof(NativeChart)));
        Add("tsugumi_chart.series", chart, &chart->Series, pointer);
        Add("tsugumi_chart.count", chart, &chart->Count, (ulong)sizeof(nuint));

        NativeTable tableValue = default;
        var table = &tableValue;
        list.Add(("tsugumi_table", 0, (ulong)sizeof(NativeTable)));
        Add("tsugumi_table.columns", table, &table->Columns, pointer);
        Add("tsugumi_table.column_count", table, &table->ColumnCount, (ulong)sizeof(nuint));
        Add("tsugumi_table.cells", table, &table->Cells, pointer);
        Add("tsugumi_table.row_count", table, &table->RowCount, (ulong)sizeof(nuint));

        NativeImage imageValue = default;
        var image = &imageValue;
        list.Add(("tsugumi_image", 0, (ulong)sizeof(NativeImage)));
        Add("tsugumi_image.media_type", image, &image->MediaType, pointer);
        Add("tsugumi_image.data", image, &image->Data, pointer);
        Add("tsugumi_image.size", image, &image->Size, (ulong)sizeof(nuint));
        Add("tsugumi_image.alt", image, &image->Alt, pointer);

        NativePoints pointsValue = default;
        var points = &pointsValue;
        list.Add(("tsugumi_points", 0, (ulong)sizeof(NativePoints)));
        Add("tsugumi_points.x", points, &points->X, pointer);
        Add("tsugumi_points.y", points, &points->Y, pointer);
        Add("tsugumi_points.z", points, &points->Z, pointer);
        Add("tsugumi_points.colors", points, &points->Colors, pointer);
        Add("tsugumi_points.count", points, &points->Count, (ulong)sizeof(nuint));

        NativeModule moduleValue = default;
        var module = &moduleValue;
        list.Add(("tsugumi_module", 0, (ulong)sizeof(NativeModule)));
        Add("tsugumi_module.name", module, &module->Name, pointer);
        Add("tsugumi_module.config_json", module, &module->Config, pointer);
        Add("tsugumi_module.blobs", module, &module->Blobs, pointer);
        Add("tsugumi_module.count", module, &module->Count, (ulong)sizeof(nuint));

        NativeCall cValue = default;
        var c = &cValue;
        list.Add(("tsugumi_call", 0, (ulong)sizeof(NativeCall)));
        Add("tsugumi_call.abi_version", c, &c->AbiVersion, sizeof(uint));
        Add("tsugumi_call.phase", c, &c->Phase, sizeof(uint));
        Add("tsugumi_call.run_id", c, &c->RunId, pointer);
        Add("tsugumi_call.card", c, &c->Card, pointer);
        Add("tsugumi_call.invocation", c, &c->Invocation, sizeof(ulong));
        Add("tsugumi_call.params", c, &c->Params, pointer);
        Add("tsugumi_call.params_size", c, &c->ParamsSize, (ulong)sizeof(nuint));
        Add("tsugumi_call.working_directory", c, &c->Work, pointer);
        Add("tsugumi_call.temporary_directory", c, &c->Temp, pointer);
        Add("tsugumi_call.output_directory", c, &c->Output, pointer);
        Add("tsugumi_call.host", c, &c->Host, pointer);
        Add("tsugumi_call.effects", c, &c->Effects, pointer);

        NativeHost hValue = default;
        var h = &hValue;
        list.Add(("tsugumi_host", 0, (ulong)sizeof(NativeHost)));
        Add("tsugumi_host.display", h, &h->Display, pointer);
        Add("tsugumi_host.log", h, &h->Log, pointer);
        Add("tsugumi_host.cancelled", h, &h->Cancelled, pointer);
        Add("tsugumi_host.remaining_ms", h, &h->Remaining, pointer);
        Add("tsugumi_host.fail", h, &h->Fail, pointer);
        Add("tsugumi_host.input", h, &h->Input, pointer);
        Add("tsugumi_host.output", h, &h->Output, pointer);
        Add("tsugumi_host.inputs_json", h, &h->InputsJson, pointer);
        Add("tsugumi_host.params_json", h, &h->ParamsJson, pointer);
        Add("tsugumi_host.input_kind", h, &h->InputKind, pointer);
        Add("tsugumi_host.output_json", h, &h->OutputJson, pointer);
        Add("tsugumi_host.fail_at", h, &h->FailAt, pointer);

        return list;
    }
}
