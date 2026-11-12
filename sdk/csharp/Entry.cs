#nullable enable
using System;
using System.Runtime.InteropServices;

namespace Tsugumi;

public static class Entry {
    private static ICard? card;
    [UnmanagedCallersOnly]
    public static int Execute(nint pointer) { try { return Invoke(pointer, false); } catch { return 1; } }
    [UnmanagedCallersOnly]
    public static int RunStart(nint pointer) { try { return Invoke(pointer, true); } catch { return 1; } }
    // Called once by host.cc before any other entry, with abi.h's layout.
    [UnmanagedCallersOnly]
    public static int CheckLayout(nint fields, nuint count) {
        try { return Layout.Check(fields, count) ? 0 : 1; }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
    private static int Invoke(nint pointer, bool start) {
        Context? context = null;
        int result = 0;
        try {
            context = new Context(pointer);
            card ??= new global::Card();
            (start ? card.OnRunStartAsync(context) : card.RunAsync(context)).AsTask().GetAwaiter().GetResult();
            result = 0;
        } catch (Exception error) {
            Console.Error.WriteLine(error);
            var kind = error is Failure failure ? failure.Kind : error is OperationCanceledException
                ? context?.RemainingMilliseconds == 0 ? "timeout" : "cancelled" : "worker_failed";
            context?.ReportFailure(kind, error.Message.Replace('\0', '\ufffd'));
            result = 1;
        } finally {
            try { context?.Dispose(); }
            catch (Exception error) {
                Console.Error.WriteLine(error);
                context?.ReportFailure("worker_failed", error.Message.Replace('\0', '\ufffd'));
                result = 1;
            }
        }
        return result;
    }
}
