# C# Cards

C# uses the same typed ports, parameters, worker and saved run records as C++ and Python. Choose **Processing (C#)** when adding a Card. Edit `src/Card.cs`, change its parameters in Card settings and run it. The [cross-language calibration demo](Demos.md#csharp-calibration) connects C++ readings to a C# transformation and a Python report.

See [Inputs, results and reports](CardGuide.md#inputs-results-and-reports) for the shared SDK contract.

## A Card

Set `language` to `csharp` in `_card.json`. `default.nix` selects the generated C# bindings and the pinned .NET SDK:

```nix
{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "csharp" ]; };
in {
  run = inputs.tsugumi.csharpCard { inherit sdk; src = ./src; };
}
```

Define `public sealed class Card : Tsugumi.ICard` in `src/Card.cs`. The read-only generated `Inputs` properties and returned `Outputs` use FlatBuffers object API classes. Port names become properties with their first character capitalized. For a port `numbers` whose schema declares `sample.Numbers`, for example:

```csharp
using System.Linq;
using System.Threading.Tasks;
using Tsugumi;

public sealed class Card : ICard {
    public ValueTask<Outputs> RunAsync(Inputs inputs, Report report, Run run) {
        var result = new Outputs();
        var input = inputs.Numbers;
        double factor = inputs.Params.Factor
            ?? throw new Failure("validation_failed", "factor is required");
        var values = input.Values.Select(value => value * factor).ToList();
        result.Numbers = new outputs.numbers.sample.NumbersT {
            Values = values, Label = input.Label
        };
        report.Scalar(values.Sum(), "sum");
        return ValueTask.FromResult(result);
    }
}
```

The exact types come from the resolved Card contract: `inputs.<port>.<namespace>.<Table>T`, `outputs.<port>.<namespace>.<Table>T`, and `params.ParamsT`. Optional ports return nullable objects; check them before use. Scalar parameters are nullable, so choose a fallback or report a missing value explicitly. C# namespaces use verbatim identifiers when needed (`@params`). Unions use FlatBuffers' generated union object API. Core validation still checks every emitted message.

Implement an instance `ICard.RunAsync` returning exactly `ValueTask<Tsugumi.Outputs>`, using this Card's generated types. Trailing roles may be omitted, but requesting `Run` requires `(Inputs inputs, Report report, Run run)`. A method with a different signature can compile without implementing the interface's entry point; the runtime diagnostic lists the candidates and why they were rejected.

`inputs.Input(port)` exposes an owned `Google.FlatBuffers.ByteBuffer` for direct generated-table access. Return typed properties on `Outputs`, or finished bytes in its dictionary (`new Outputs { ["port"] = builder.SizedByteArray() }`). The adapter emits the returned result after awaiting `RunAsync`. For repeated or incremental results, return `Outputs.Stream(IAsyncEnumerable<Outputs>)`; each yielded batch is sent before advancing the iterator. File/Directory properties contain their path messages, including files written beneath `run.OutputDirectory`.


C# Cards use the Card's own system; `csharpCard` rejects another `system` under emulation. External NuGet dependencies require an explicit lock and hash, and their native libraries are used as published. [NuGet packages](#nuget-packages)

## Tasks, state and lifetime

The worker waits for the `ValueTask` returned by `RunAsync`. Await every operation that belongs to the invocation, including child Tasks. Fire-and-forget Tasks are not joined and their later exceptions cannot become that invocation's structured failure. Native callbacks remain valid only until the awaited invocation and SDK finalization finish. A saved `Report` or `Run` rejects later log, display, cancellation-status or deadline calls with `ObjectDisposedException`; managed input copies may still be read. The SDK pins output and display buffers only for the synchronous native callback, whose host copies the bytes before returning.

Pass `run.CancellationToken` to cancellable asynchronous work. The SDK polls the worker cancellation flag and deadline; the worker remains the final enforcement boundary for code that ignores cancellation. `RemainingMilliseconds` is `-1` without a deadline. Exceptions become structured worker failures; `new Failure(kind, message)` chooses a `FailureKind` name, and cancelled Tasks report cancellation or timeout. Exceptions do not cross the native ABI.

One `Card` instance and one .NET runtime live for the resident worker's lifetime. `OnRunStartAsync(Inputs inputs, Report report, Run run)` runs once at each run's start, before that run's invocations. Use it to reset run-local state. `reuseWorker: false` starts a fresh process/runtime per invocation. Process exit frees the CLR; the SDK does not attempt to unload it. After failed or cancelled invocations, the scheduler's existing worker policy applies.

## Files, logs and displays

File and Directory ports use their generated message's `Value` path, just as in other SDKs. Read an input path directly; write outputs below `context.OutputDirectory` and emit the generated path message. `InputKind(port)` distinguishes `File`, `Directory` and `FlatBuffer`. `WorkingDirectory` and `TemporaryDirectory` follow the Card's existing filesystem policy. Retain an output by selecting `artifactOutput` and enabling `retainArtifact` in Card settings.

The SDK provides `Log`, `Text`, `Markdown`, `Scalar`, `Table`, `Image`, `LineChart`, `Points` and `Module`. Text is strict UTF-8 and rejects embedded NUL characters. Display arrays must have matching lengths; rejected native displays fail the invocation. Image bytes and FlatBuffer bytes are binary, not text. The same sandbox/network restrictions apply to .NET and NuGet code as to other Cards.

## The native boundary

`sdk/csharp/InvocationScope.cs` declares the Card ABI's structs (`tsugumi_call`, `tsugumi_value`, `tsugumi_series` in `sdk/include/tsugumi/sdk/abi.h`) by hand. Before the first call, the C++ host passes the compiler's `offsetof`/`sizeof` of every field to the managed SDK, which compares them with its own layout; any field missing, moved or resized on either side fails `tsugumi_load` with the field named in the Card's log. `tests/static/test_csharp_abi.py` checks field order and callback signatures from the sources without .NET. Extending the ABI (a new callback at the end of `tsugumi_call`) means adding the field to `abi.h`, `host.cc`'s layout table, `NativeCall` and `Layout.cs`.

## NuGet packages

The project's pinned `nixpkgs` supplies .NET 10, its runtime/host headers, and FlatBuffers runtime sources matching `flatc`. A Card without `lock` has no external packages. A Card that uses NuGet packages keeps two things in its own folder, and both are required:

```nix
run = inputs.tsugumi.csharpCard {
  inherit sdk;
  src = ./src;
  lock = ./packages.lock.json;  # NuGet's own lock file
  hash = "sha256-...";          # NAR hash of the .nupkg files the lock names
};
```

- **`lock`** is the `packages.lock.json` NuGet writes. It is the only list of packages: its `Direct` entries become the Card's package references (with the recorded `requested` ranges), and every package it resolves to is used, transitive ones included. The lock must have a section for the Card's framework (`net10.0`).
- **`hash`** is the NAR hash (sha256, `nix hash path`) of one folder: the `.nupkg` files of every package the lock resolves to, as nuget.org publishes them, named `<id>.<version>.nupkg` in lower case. Nix fetches that folder as the fixed-output derivation `<name>-nuget-lock-<first 16 hex digits of the lock file's sha256>` and rejects anything else. Because the name follows the lock, editing the lock without updating `hash` fails instead of reusing packages fetched for the old lock.

The build then runs `dotnet restore --locked-mode` without network, with that folder as its only package source (no user, machine or fallback NuGet settings). Locked mode also checks each package against the lock's `contentHash`. Nothing is fetched or restored while a Card runs. A lock without `hash`, a `hash` without lock, or a lock without the Card's framework is an evaluation error that says what to add.

### Making or changing a lock

Nothing makes the lock for you. To add or update packages, deliberately:

1. In an empty folder, write a project with the Card's framework and the packages you want:

   ```xml
   <Project Sdk="Microsoft.NET.Sdk">
     <PropertyGroup>
       <TargetFramework>net10.0</TargetFramework>
       <RestorePackagesWithLockFile>true</RestorePackagesWithLockFile>
     </PropertyGroup>
     <ItemGroup>
       <PackageReference Include="Newtonsoft.Json" Version="13.0.3" />
     </ItemGroup>
   </Project>
   ```

2. Run `nix shell nixpkgs#dotnet-sdk_10 -c dotnet restore` there (this one step uses the network) and copy the `packages.lock.json` it writes next to the Card's `default.nix`. Review the versions it resolved.
3. Set `hash = pkgs.lib.fakeHash;` and run the Card. Its build fails with `hash mismatch in fixed-output derivation '…-nuget-lock-…'`; the line `got: sha256-…` in the Card's **build log** is the hash of the packages that lock names. The build log is part of the Card's own log in the app (Nix evaluation and build output appear there), and the `build` stream of `LogRange` for that Card; it is not in the worker's log.
4. Write that hash into `hash` and run again. The build now restores offline. Commit `packages.lock.json` and `default.nix` together.

`passthru.nugetPackages` of the Card's package is the fetched folder, for checking the hash by hand with `nix hash path`.

## Why runtime hosting

The Card exports the existing `tsugumi_load`, `tsugumi_main` and `tsugumi_run_start` ABI. A small C++ library loads `hostfxr`, initializes the pinned runtime and calls managed `[UnmanagedCallersOnly]` entry points. It adds no separate RPC or subprocess protocol. CLR diagnostics sockets are disabled; ordinary logs and structured failures remain available.

The documented platform constraints are in Microsoft's [runtime hosting tutorial](https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting). FlatBuffers' [language support matrix](https://flatbuffers.dev/support/) describes the generated C# object API.
