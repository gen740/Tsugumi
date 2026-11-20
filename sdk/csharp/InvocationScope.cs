#nullable enable
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Google.FlatBuffers;

namespace Tsugumi;

public sealed class Failure : Exception {
    public string Kind { get; }
    public Failure(string kind, string message) : base(message) => Kind = kind;
}

public enum PortKind : uint { File = 1, Directory = 2, FlatBuffer = 3 }

// The native structs mirror sdk/include/tsugumi/sdk/abi.h
// field by field, in its order. Layout.cs checks them against the compiled
// abi.h when the Card loads; a new abi.h field is added in all three places.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeSeries {
    internal byte* Id;
    internal byte* Label;
    internal double* X;
    internal double* Y;
    internal nuint Count;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeBlob {
    internal byte* Name;
    internal byte* Data;
    internal nuint Size;
}

internal enum NativePhase : uint { Prepare, Load, RunStart, Run }

internal enum NativeDisplayKind : uint {
    Text, Markdown, Scalar, LineChart, Table, Image, Points, Module
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeScalar {
    internal double Value;
    internal byte* Label;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeChart {
    internal NativeSeries* Series;
    internal nuint Count;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeTable {
    internal byte** Columns;
    internal nuint ColumnCount;
    internal byte** Cells;
    internal nuint RowCount;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeImage {
    internal byte* MediaType;
    internal byte* Data;
    internal nuint Size;
    internal byte* Alt;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativePoints {
    internal float* X;
    internal float* Y;
    internal float* Z;
    internal uint* Colors;
    internal nuint Count;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeModule {
    internal byte* Name;
    internal byte* Config;
    internal NativeBlob* Blobs;
    internal nuint Count;
}

public sealed record Series(string Id, string Label, double[] X, double[] Y);

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCall {
    internal uint AbiVersion;
    internal NativePhase Phase;
    internal byte* RunId;
    internal byte* Card;
    internal ulong Invocation;
    internal byte* Params;
    internal nuint ParamsSize;
    internal byte* Work;
    internal byte* Temp;
    internal byte* Output;
    internal void* Host;
    internal NativeHost* Effects;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeHost {
    internal delegate* unmanaged[Cdecl]<void*, NativeDisplayKind, void*, int> Display;
    internal delegate* unmanaged[Cdecl]<void*, byte*, void> Log;
    internal delegate* unmanaged[Cdecl]<void*, int> Cancelled;
    internal delegate* unmanaged[Cdecl]<void*, long> Remaining;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*, void> Fail;
    internal delegate* unmanaged[Cdecl]<void*, byte*, nuint*, byte*> Input;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*, nuint, int> Output;
    internal delegate* unmanaged[Cdecl]<void*, byte*> InputsJson;
    internal delegate* unmanaged[Cdecl]<void*, byte*> ParamsJson;
    internal delegate* unmanaged[Cdecl]<void*, byte*, uint> InputKind;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*, int> OutputJson;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*, byte*, uint, byte*, void> FailAt;
}

/// Input/param bytes are managed copies. Native callbacks are valid only while
/// the awaited Card invocation is active; a retained InvocationScope rejects later use.
internal sealed class InvocationScope {
    private readonly NativeCall call;
    private readonly object gate = new();
    private readonly byte[] parameters;
    private readonly CancellationTokenSource cancellation = new();
    private readonly CancellationTokenSource watcherStop = new();
    private readonly Task watcher;
    private bool active = true;
    public string RunId { get; }
    public string Card { get; }
    public ulong Invocation { get; }
    public string WorkingDirectory { get; }
    public string TemporaryDirectory { get; }
    public string OutputDirectory { get; }
    public CancellationToken CancellationToken => cancellation.Token;

    internal unsafe InvocationScope(nint pointer) {
        call = *(NativeCall*)pointer;
        if (call.AbiVersion != Abi.Version) throw new InvalidOperationException($"C# SDK requires Card ABI {Abi.Version}");
        RunId = String(call.RunId); Card = String(call.Card); Invocation = call.Invocation;
        WorkingDirectory = String(call.Work); TemporaryDirectory = String(call.Temp); OutputDirectory = String(call.Output);
        parameters = Copy(call.Params, call.ParamsSize);
        watcher = WatchAsync();
    }
    private static unsafe string String(byte* value) => Marshal.PtrToStringUTF8((nint)value) ?? "";
    private static unsafe byte[] Copy(byte* value, nuint size) {
        var data = new byte[checked((int)size)];
        if (data.Length > 0) Marshal.Copy((nint)value, data, 0, data.Length);
        return data;
    }
    private static byte[] Utf8(string value) {
        if (value.Contains('\0')) throw new ArgumentException("Text cannot contain a NUL character");
        return new UTF8Encoding(false, true).GetBytes(value + '\0');
    }
    private void CheckActive() { if (!active) throw new ObjectDisposedException(nameof(InvocationScope)); }
    public unsafe ByteBuffer? Input(string port) {
        lock (gate) {
            CheckActive();
            fixed (byte* name = Utf8(port)) {
                nuint size;
                byte* bytes = call.Effects->Input(call.Host, name, &size);
                return bytes == null ? null : new ByteBuffer(Copy(bytes, size));
            }
        }
    }
    internal unsafe void Send(string port, byte[] bytes) {
        lock (gate) {
            CheckActive();
            fixed (byte* name = Utf8(port), data = bytes) {
                if (call.Effects->Output(call.Host, name, data, (nuint)bytes.Length) != 0)
                    throw new Failure("output_invalid", port);
            }
        }
    }
    internal unsafe bool IsRunStart => call.Phase == NativePhase.RunStart;
    public ByteBuffer Params => new((byte[])parameters.Clone());
    public unsafe long RemainingMilliseconds {
        get { lock (gate) { CheckActive(); return call.Effects->Remaining(call.Host); } }
    }
    public unsafe bool IsCancelled {
        get { lock (gate) { CheckActive(); return call.Effects->Cancelled(call.Host) != 0; } }
    }
    private async Task WatchAsync() {
        try {
            while (true) {
                if (IsCancelled || RemainingMilliseconds == 0) cancellation.Cancel();
                await Task.Delay(10, watcherStop.Token).ConfigureAwait(false);
            }
        } catch (OperationCanceledException) when (watcherStop.IsCancellationRequested) { }
    }
    public unsafe void Log(string message) { lock (gate) { CheckActive(); fixed (byte* text = Utf8(message)) call.Effects->Log(call.Host, text); } }
    public unsafe void Text(string message) { lock (gate) { CheckActive(); fixed (byte* text = Utf8(message)) CheckDisplay(call.Effects->Display(call.Host, NativeDisplayKind.Text, text)); } }
    public unsafe void Markdown(string message) { lock (gate) { CheckActive(); fixed (byte* text = Utf8(message)) CheckDisplay(call.Effects->Display(call.Host, NativeDisplayKind.Markdown, text)); } }
    public unsafe void Scalar(double value, string label) {
        lock (gate) { CheckActive(); fixed (byte* text = Utf8(label)) {
            var data = new NativeScalar { Value = value, Label = text };
            CheckDisplay(call.Effects->Display(call.Host, NativeDisplayKind.Scalar, &data));
        } }
    }
    public unsafe void Image(byte[] data, string mediaType = "image/png", string alt = "") {
        lock (gate) { CheckActive(); fixed (byte* bytes = data, type = Utf8(mediaType), text = Utf8(alt)) {
            var image = new NativeImage { MediaType = type, Data = bytes, Size = (nuint)data.Length, Alt = text };
            CheckDisplay(call.Effects->Display(call.Host, NativeDisplayKind.Image, &image));
        } }
    }
    public unsafe void Module(string name, string json) {
        lock (gate) { CheckActive(); fixed (byte* n = Utf8(name), data = Utf8(json)) {
            var module = new NativeModule { Name = n, Config = data };
            CheckDisplay(call.Effects->Display(call.Host, NativeDisplayKind.Module, &module));
        } }
    }
    public unsafe void Points(float[] x, float[] y, float[] z, uint[]? colors = null) {
        if (x.Length != y.Length || x.Length != z.Length || (colors != null && colors.Length != x.Length))
            throw new ArgumentException("Point arrays must have equal lengths");
        lock (gate) { CheckActive(); fixed (float* a = x, b = y, c = z) fixed (uint* d = colors) {
            var points = new NativePoints { X = a, Y = b, Z = c, Colors = d, Count = (nuint)x.Length };
            CheckDisplay(call.Effects->Display(call.Host, NativeDisplayKind.Points, &points));
        } }
    }
    public unsafe void LineChart(params Series[] series) {
        var pins = new List<GCHandle>();
        nint Pin(object value) { var pin = GCHandle.Alloc(value, GCHandleType.Pinned); pins.Add(pin); return pin.AddrOfPinnedObject(); }
        try {
            var values = new NativeSeries[series.Length];
            for (int i = 0; i < values.Length; i++) {
                var item = series[i];
                if (item.X.Length != item.Y.Length) throw new ArgumentException("Series arrays must have equal lengths");
                values[i] = new NativeSeries { Id = (byte*)Pin(Utf8(item.Id)), Label = (byte*)Pin(Utf8(item.Label)),
                    X = (double*)Pin(item.X), Y = (double*)Pin(item.Y), Count = (nuint)item.X.Length };
            }
            lock (gate) { CheckActive(); fixed (NativeSeries* data = values) {
                var chart = new NativeChart { Series = data, Count = (nuint)values.Length };
                CheckDisplay(call.Effects->Display(call.Host, NativeDisplayKind.LineChart, &chart));
            } }
        } finally { foreach (var pin in pins) pin.Free(); }
    }
    private static void CheckDisplay(int status) {
        if (status != 0) throw new Failure("output_invalid", "Display was rejected by the worker");
    }
    public unsafe void Table(string[] columns, string[][] rows) {
        var pins = new List<GCHandle>();
        nint Pin(string value) { var pin = GCHandle.Alloc(Utf8(value), GCHandleType.Pinned); pins.Add(pin); return pin.AddrOfPinnedObject(); }
        try {
            var names = Array.ConvertAll(columns, Pin);
            var cells = new List<nint>();
            foreach (var row in rows) {
                if (row.Length != columns.Length) throw new ArgumentException("Each row must match the column count");
                foreach (var value in row) cells.Add(Pin(value));
            }
            var array = cells.ToArray();
            lock (gate) { CheckActive(); fixed (nint* n = names, v = array) {
                var table = new NativeTable { Columns = (byte**)n, ColumnCount = (nuint)names.Length,
                    Cells = (byte**)v, RowCount = (nuint)rows.Length };
                CheckDisplay(call.Effects->Display(call.Host, NativeDisplayKind.Table, &table));
            } }
        } finally { foreach (var pin in pins) pin.Free(); }
    }
    internal unsafe void ReportFailure(string kind, string message) {
        // Entry may report a finalization error after disabling user callbacks.
        lock (gate) { fixed (byte* k = Utf8(kind), text = Utf8(message)) call.Effects->Fail(call.Host, k, text); }
    }
    internal void Dispose() {
        watcherStop.Cancel();
        try { watcher.GetAwaiter().GetResult(); }
        finally {
            lock (gate) active = false;
            watcherStop.Dispose();
            cancellation.Dispose();
        }
    }
}

public sealed class Report {
    private readonly InvocationScope scope;
    internal Report(InvocationScope scope) => this.scope = scope;
    public void Log(string message) => scope.Log(message);
    public void Text(string message) => scope.Text(message);
    public void Markdown(string message) => scope.Markdown(message);
    public void Scalar(double value, string label) => scope.Scalar(value, label);
    public void Image(byte[] data, string mediaType = "image/png", string alt = "") => scope.Image(data, mediaType, alt);
    public void Module(string name, string json) => scope.Module(name, json);
    public void Points(float[] x, float[] y, float[] z, uint[]? colors = null) => scope.Points(x, y, z, colors);
    public void LineChart(params Series[] series) => scope.LineChart(series);
    public void Table(string[] columns, string[][] rows) => scope.Table(columns, rows);
}

public sealed class Run {
    private readonly InvocationScope scope;
    internal Run(InvocationScope value) => scope = value;
    public string RunId => scope.RunId;
    public string Card => scope.Card;
    public ulong Invocation => scope.Invocation;
    public string WorkingDirectory => scope.WorkingDirectory;
    public string TemporaryDirectory => scope.TemporaryDirectory;
    public string OutputDirectory => scope.OutputDirectory;
    public CancellationToken CancellationToken => scope.CancellationToken;
    public bool IsCancelled => scope.IsCancelled;
    public long RemainingMilliseconds => scope.RemainingMilliseconds;
}
