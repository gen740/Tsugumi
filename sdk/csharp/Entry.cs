#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace Tsugumi;

public static class Entry {
    private static ICard? card;

    // Dispatch stays on ICard. Reflection here only explains why an author's
    // method did not implement any of its supported overloads.
    internal static Exception MissingRunAsync(ICard instance) {
        var type = instance.GetType();
        var candidates = type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static)
            .Where(method => method.Name == "RunAsync" || method.Name.EndsWith(".RunAsync", StringComparison.Ordinal))
            .OrderBy(method => method.ToString(), StringComparer.Ordinal);
        var found = new List<string>();
        Type[] roles = [typeof(Inputs), typeof(Report), typeof(Run)];
        foreach (var method in candidates) {
            var reasons = new List<string>();
            if (method.IsStatic) reasons.Add("must be an instance method");
            if (!method.IsPublic) reasons.Add("must be public to implement ICard implicitly");
            if (method.IsGenericMethod) reasons.Add("must not be a generic method");
            var parameters = method.GetParameters();
            if (parameters.Length < 1 || parameters.Length > roles.Length)
                reasons.Add("must take 1 to 3 parameters in order: Inputs, Report, Run");
            for (var i = 0; i < Math.Min(parameters.Length, roles.Length); i++) {
                if (parameters[i].ParameterType != roles[i])
                    reasons.Add($"parameter {i + 1}: {TypeMismatch(parameters[i].ParameterType, roles[i])}");
            }
            if (method.ReturnType != typeof(ValueTask<Outputs>))
                reasons.Add($"return type: {TypeMismatch(method.ReturnType, typeof(ValueTask<Outputs>))}");
            if (reasons.Count == 0) reasons.Add("does not implement this Card's ICard.RunAsync overload");
            var visibility = method.IsPublic ? "public" : method.IsPrivate ? "private" : "non-public";
            var signature = $"{visibility} {(method.IsStatic ? "static" : "instance")} {TypeName(method.ReturnType)} "
                + $"{method.DeclaringType?.FullName}.{method.Name}("
                + string.Join(", ", parameters.Select(parameter => $"{TypeName(parameter.ParameterType)} {parameter.Name}")) + ")";
            found.Add($"{signature}: {string.Join("; ", reasons)}");
        }
        return new NotImplementedException(
            $"Define public instance ValueTask<Tsugumi.Outputs> RunAsync(inputs[, report[, run]]) on {type.FullName}. "
            + "Use this Card's generated Tsugumi.Inputs and Tsugumi.Outputs; Run requires the third parameter after Report. "
            + "Found: " + (found.Count == 0 ? "no RunAsync methods" : string.Join(" | ", found)));
    }

    private static string TypeName(Type type) {
        if (!type.IsGenericType) return type.FullName ?? type.Name;
        var name = type.GetGenericTypeDefinition().FullName ?? type.Name;
        return name.Split('`')[0] + "<" + string.Join(", ", type.GetGenericArguments().Select(TypeName)) + ">";
    }

    private static string TypeMismatch(Type actual, Type expected) {
        var detail = TypeName(actual) == TypeName(expected)
            ? $" (actual identity: {actual.AssemblyQualifiedName}; expected identity: {expected.AssemblyQualifiedName})" : "";
        return $"expected {TypeName(expected)}, found {TypeName(actual)}{detail}";
    }

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
        InvocationScope? scope = null;
        int result = 0;
        try {
            scope = new InvocationScope(pointer);
            card ??= new global::Card();
            var inputs = new Inputs(scope);
            var report = new Report(scope);
            var run = new Run(scope);
            var outputs = (start ? card.OnRunStartAsync(inputs, report, run) : card.RunAsync(inputs, report, run)).AsTask().GetAwaiter().GetResult();
            outputs.SendAsync(scope).AsTask().GetAwaiter().GetResult();
            result = 0;
        } catch (Exception error) {
            Console.Error.WriteLine(error);
            var kind = error is Failure failure ? failure.Kind : error is OperationCanceledException
                ? scope?.RemainingMilliseconds == 0 ? "timeout" : "cancelled" : "worker_failed";
            scope?.ReportFailure(kind, error.Message.Replace('\0', '\ufffd'));
            result = 1;
        } finally {
            try { scope?.Dispose(); }
            catch (Exception error) {
                Console.Error.WriteLine(error);
                scope?.ReportFailure("worker_failed", error.Message.Replace('\0', '\ufffd'));
                result = 1;
            }
        }
        return result;
    }
}
