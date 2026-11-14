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

public interface ICard {
    ValueTask<Outputs> RunAsync(Inputs inputs) => throw Entry.MissingRunAsync(this);
    ValueTask<Outputs> RunAsync(Inputs inputs, Report report) => RunAsync(inputs);
    ValueTask<Outputs> RunAsync(Inputs inputs, Report report, Run run) => RunAsync(inputs, report);
    ValueTask<Outputs> OnRunStartAsync(Inputs inputs) => ValueTask.FromResult(new Outputs());
    ValueTask<Outputs> OnRunStartAsync(Inputs inputs, Report report) => OnRunStartAsync(inputs);
    ValueTask<Outputs> OnRunStartAsync(Inputs inputs, Report report, Run run) => OnRunStartAsync(inputs, report);
}

public enum PortKind : uint { File = 1, Directory = 2, FlatBuffer = 3 }

// NativeValue, NativeSeries and NativeCall mirror sdk/include/tsugumi/sdk/abi.h
// field by field, in its order. Layout.cs checks them against the compiled
// abi.h when the Card loads; a new abi.h field is added in all three places.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeValue {
    internal byte* Name;
    internal uint Kind;
    internal byte* Data;
    internal nuint Size;
}

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

public sealed record Series(string Id, string Label, double[] X, double[] Y);

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCall {
    internal uint AbiVersion;
    internal byte* RunId;
    internal byte* Card;
    internal ulong Invocation;
    internal NativeValue* Inputs;
    internal nuint InputCount;
    internal byte* Params;
    internal nuint ParamsSize;
    internal byte* Work;
    internal byte* Temp;
    internal byte* Output;
    internal void* Host;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*, nuint, int> Emit;
    internal delegate* unmanaged[Cdecl]<void*, byte*, int> Text;
    internal delegate* unmanaged[Cdecl]<void*, double, byte*, int> Scalar;
    internal delegate* unmanaged[Cdecl]<void*, NativeSeries*, nuint, int> LineChart;
    internal delegate* unmanaged[Cdecl]<void*, byte*, void> Log;
    internal delegate* unmanaged[Cdecl]<void*, int> Cancelled;
    internal delegate* unmanaged[Cdecl]<void*, long> Remaining;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*, void> Fail;
    internal delegate* unmanaged[Cdecl]<void*, byte**, nuint, byte**, nuint, int> Table;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*, nuint, byte*, int> Image;
    internal delegate* unmanaged[Cdecl]<void*, float*, float*, float*, uint*, nuint, int> Points;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*, NativeBlob*, nuint, int> Module;
    internal delegate* unmanaged[Cdecl]<void*, byte*, int> Markdown;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*> InputJson;
    internal delegate* unmanaged[Cdecl]<void*, byte*> ParamsJson;
    internal delegate* unmanaged[Cdecl]<void*, byte*, byte*, int> EmitJson;
    internal delegate* unmanaged[Cdecl]<void*, byte*, int> DisplayJson;
    internal delegate* unmanaged[Cdecl]<void*, int> FinishOutputs;
}

/// Input/param bytes are managed copies. Native callbacks are valid only while
/// the awaited Card invocation is active; a retained InvocationScope rejects later use.
internal sealed class InvocationScope {
    private readonly NativeCall call;
    private readonly object gate = new();
    private readonly Dictionary<string, (PortKind Kind, byte[] Bytes)> inputs = new();
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
        for (nuint i = 0; i < call.InputCount; i++) {
            var input = call.Inputs[i];
            inputs.Add(String(input.Name), ((PortKind)input.Kind, Copy(input.Data, input.Size)));
        }
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
    public bool HasInput(string port) => inputs.ContainsKey(port);
    public PortKind InputKind(string port) => inputs[port].Kind;
    public ByteBuffer Input(string port) => new(inputs.TryGetValue(port, out var value) ? (byte[])value.Bytes.Clone() : throw new Failure("validation_failed", $"Input {port} is absent"));
    public ByteBuffer Params => new((byte[])parameters.Clone());
    public unsafe long RemainingMilliseconds {
        get { lock (gate) { CheckActive(); return call.Remaining(call.Host); } }
    }
    public unsafe bool IsCancelled {
        get { lock (gate) { CheckActive(); return call.Cancelled(call.Host) != 0; } }
    }
    private async Task WatchAsync() {
        try {
            while (true) {
                if (IsCancelled || RemainingMilliseconds == 0) cancellation.Cancel();
                await Task.Delay(10, watcherStop.Token).ConfigureAwait(false);
            }
        } catch (OperationCanceledException) when (watcherStop.IsCancellationRequested) { }
    }
    public unsafe void Output(string port, FlatBufferBuilder builder) => OutputBytes(port, builder.SizedByteArray());
    public unsafe void OutputBytes(string port, byte[] data) {
        lock (gate) {
            CheckActive();
            fixed (byte* name = Utf8(port), bytes = data)
                if (call.Emit(call.Host, name, bytes, (nuint)data.Length) != 0) throw new Failure("output_invalid", $"Output {port} was rejected");
        }
    }
    public unsafe void FinishOutputs() {
        lock (gate) {
            CheckActive();
            if (call.FinishOutputs(call.Host) != 0) throw new Failure("output_invalid", "Required outputs were not emitted");
        }
    }
    public unsafe void Log(string message) { lock (gate) { CheckActive(); fixed (byte* text = Utf8(message)) call.Log(call.Host, text); } }
    public unsafe void Text(string message) { lock (gate) { CheckActive(); fixed (byte* text = Utf8(message)) CheckDisplay(call.Text(call.Host, text)); } }
    public unsafe void Markdown(string message) { lock (gate) { CheckActive(); fixed (byte* text = Utf8(message)) CheckDisplay(call.Markdown(call.Host, text)); } }
    public unsafe void Scalar(double value, string label) { lock (gate) { CheckActive(); fixed (byte* text = Utf8(label)) CheckDisplay(call.Scalar(call.Host, value, text)); } }
    public unsafe void Image(byte[] data, string mediaType = "image/png", string alt = "") {
        lock (gate) { CheckActive(); fixed (byte* bytes = data, type = Utf8(mediaType), text = Utf8(alt)) CheckDisplay(call.Image(call.Host, type, bytes, (nuint)data.Length, text)); }
    }
    public unsafe void Module(string name, string json) { lock (gate) { CheckActive(); fixed (byte* n = Utf8(name), data = Utf8(json)) CheckDisplay(call.Module(call.Host, n, data, null, 0)); } }
    public unsafe void Points(float[] x, float[] y, float[] z, uint[]? colors = null) {
        if (x.Length != y.Length || x.Length != z.Length || (colors != null && colors.Length != x.Length))
            throw new ArgumentException("Point arrays must have equal lengths");
        lock (gate) { CheckActive(); fixed (float* a = x, b = y, c = z) fixed (uint* d = colors)
            CheckDisplay(call.Points(call.Host, a, b, c, d, (nuint)x.Length)); }
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
            lock (gate) { CheckActive(); fixed (NativeSeries* data = values) CheckDisplay(call.LineChart(call.Host, data, (nuint)values.Length)); }
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
            lock (gate) { CheckActive(); fixed (nint* n = names, v = array) CheckDisplay(call.Table(call.Host, (byte**)n, (nuint)names.Length, (byte**)v, (nuint)rows.Length)); }
        } finally { foreach (var pin in pins) pin.Free(); }
    }
    internal unsafe void ReportFailure(string kind, string message) {
        // Entry may report a finalization error after disabling user callbacks.
        lock (gate) { fixed (byte* k = Utf8(kind), text = Utf8(message)) call.Fail(call.Host, k, text); }
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
