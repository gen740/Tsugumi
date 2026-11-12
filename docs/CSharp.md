# C# Cards

C# uses the same typed ports, parameters, worker and saved run records as C++ and Python. Choose **Processing (C#)** when adding a Card. Edit `src/Card.cs`, change its parameters in Card settings and run it. The [cross-language calibration demo](Demos.md#csharp-calibration) connects C++ readings to a C# transformation and a Python report.

## A Card

Set `language` to `csharp` in `_card.json`. `default.nix` selects the generated C# bindings and the pinned .NET SDK:

```nix
{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "csharp" ]; };
in {
  run = inputs.tsugumi.csharpCard { inherit sdk; src = ./src; };
}
```

Define `public sealed class Card : Tsugumi.ICard` in `src/Card.cs`. The generated `Context.Inputs` and `Context.Outputs` use FlatBuffers object API classes. Port names become properties/methods with their first character capitalized. For a port `numbers` whose schema declares `sample.Numbers`, for example:

```csharp
using System.Linq;
using System.Threading.Tasks;
using Tsugumi;

public sealed class Card : ICard {
    public ValueTask RunAsync(Context context) {
        var input = context.Inputs.Numbers;
        double factor = context.Parameters.Factor
            ?? throw new Failure("validation_failed", "factor is required");
        var values = input.Values.Select(value => value * factor).ToList();
        context.Outputs.Numbers(new outputs.numbers.sample.NumbersT {
            Values = values, Label = input.Label
        });
        context.DisplayScalar(values.Sum(), "sum");
        return ValueTask.CompletedTask;
    }
}
```

The exact types come from the resolved Card contract: `inputs.<port>.<namespace>.<Table>T`, `outputs.<port>.<namespace>.<Table>T`, and `params.ParamsT`. Optional ports return nullable objects; check them before use. Scalar parameters are nullable, so choose a fallback or report a missing value explicitly. C# namespaces use verbatim identifiers when needed (`@params`). Unions use FlatBuffers' generated union object API. Core validation still checks every emitted message.

`context.Input(port)` and `context.Params` expose owned `Google.FlatBuffers.ByteBuffer` copies for direct generated-table access. `Output(port, builder)` and `OutputBytes(port, bytes)` provide lower-level emission. Each output port is emitted independently; returning from `RunAsync` does not implicitly emit anything.

## Tasks, state and lifetime

The worker waits for the `ValueTask` returned by `RunAsync`. Await every operation that belongs to the invocation, including child Tasks. Fire-and-forget Tasks are not joined and their later exceptions cannot become that invocation's structured failure. Native callbacks remain valid only until the awaited invocation and SDK finalization finish. A saved `Context` rejects later log, display, output, cancellation-status or deadline calls with `ObjectDisposedException`; managed input copies may still be read. The SDK pins output and display buffers only for the synchronous native callback, whose host copies the bytes before returning.

Pass `context.CancellationToken` to cancellable asynchronous work. The SDK polls the worker cancellation flag and deadline; the worker remains the final enforcement boundary for code that ignores cancellation. `RemainingMilliseconds` is `-1` without a deadline. Exceptions become structured worker failures; `new Failure(kind, message)` chooses a `FailureKind` name, and cancelled Tasks report cancellation or timeout. Exceptions do not cross the native ABI.

One `Card` instance and one .NET runtime live for the resident worker's lifetime. `OnRunStartAsync(Context)` runs once at each run's start, before that run's invocations. Use it to reset run-local state. `reuseWorker: false` starts a fresh process/runtime per invocation. Process exit frees the CLR; the SDK does not attempt to unload it. After failed or cancelled invocations, the scheduler's existing worker policy applies.

## Files, logs and displays

File and Directory ports use their generated message's `Value` path, just as in other SDKs. Read an input path directly; write outputs below `context.OutputDirectory` and emit the generated path message. `InputKind(port)` distinguishes `File`, `Directory` and `FlatBuffer`. `WorkingDirectory` and `TemporaryDirectory` follow the Card's existing filesystem policy. Retain an output by selecting `artifactOutput` and enabling `retainArtifact` in Card settings.

The SDK provides `Log`, `DisplayText`, `DisplayMarkdown`, `DisplayScalar`, `DisplayTable`, `DisplayImage`, `DisplayLineChart`, `DisplayPoints` and `DisplayModule`. Text is strict UTF-8 and rejects embedded NUL characters. Display arrays must have matching lengths; rejected native displays fail the invocation. Image bytes and FlatBuffer bytes are binary, not text. The same sandbox/network restrictions apply to .NET and NuGet code as to other Cards.

## Reproducible NuGet dependencies

The project's pinned `nixpkgs` supplies .NET 10, its runtime/host headers, and FlatBuffers runtime sources matching `flatc`. There is no NuGet download during a Card invocation. Package versions and all transitive package hashes belong in the Card source:

```nix
run = inputs.tsugumi.csharpCard {
  inherit sdk;
  src = ./src;
  packages = [{ name = "Newtonsoft.Json"; version = "13.0.3"; }];
  nugetDeps = ./nuget-deps.json;
};
```

`nuget-deps.json` is the pinned nixpkgs `buildDotnetModule` dependency format:

```json
[{"pname":"Newtonsoft.Json","version":"13.0.3","hash":"sha256-hy/BieY4qxBWVVsDqqOPaLy1QobiIapkbrESm6v2PHc="}]
```

Include every transitive package. The resulting derivation exposes `passthru.fetch-deps`, from `buildDotnetModule`, for regenerating this file when deliberately updating dependencies. Review its versions and hashes before rebuilding. Supplying packages without a dependency file is an evaluation error. Restore/build uses only the declared packages and the pinned SDK.

## Why runtime hosting

The Card exports the existing `tsugumi_load`, `tsugumi_main` and `tsugumi_run_start` ABI. A small C++ library loads `hostfxr`, initializes the pinned runtime and calls managed `[UnmanagedCallersOnly]` entry points. It adds no separate RPC or subprocess protocol. CLR diagnostics sockets are disabled; ordinary logs and structured failures remain available.

A local comparison on macOS arm64 used .NET SDK 10.0.403/runtime 10.0.14, seven alternating warm-cache trials of the same `Add(int)` entry point, and a separately compiled assembly loaded through `Assembly.LoadFrom` and reflection:

| Implementation | Median startup to loaded entry | Median repeated call (100,000 calls) | Dynamic assembly invocation |
| --- | ---: | ---: | --- |
| Process-owned `hostfxr` | 37.8 ms | 3.357 ns | succeeded in 7/7 trials |
| NativeAOT shared library | 3.331 ms | 2.803 ns | `PlatformNotSupportedException` in 7/7 trials |
| Separate `dotnet` process | 40.249 ms to process exit | not measured | not measured |

These are a tiny warm-cache probe, not end-to-end Card throughput or cold-start claims. NativeAOT reduced warm startup, but restricting dynamic assembly loading and trimming/reflection would exclude existing managed libraries. Runtime hosting preserves those capabilities and amortizes initialization across resident invocations. A separate process would pay startup on each invocation or require a second resident protocol; the existing Card ABI already supplies the ownership, cancellation and lifetime boundary needed here.

The documented platform constraints are in Microsoft's [runtime hosting tutorial](https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting), [NativeAOT deployment guide](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/) and [NativeAOT native-library guide](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/libraries). FlatBuffers' [language support matrix](https://flatbuffers.dev/support/) describes the generated C# object API.
