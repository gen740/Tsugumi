using System.Linq;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Tsugumi;

public sealed class Card : ICard {
    public ValueTask<Outputs> RunAsync(Inputs inputs, Report report) {
        var result = new Outputs();
        var received = inputs.Numbers;
        double factor = inputs.Params.Factor
            ?? throw new Failure("validation_failed", "factor is required");
        var values = received.Values.Select(value => value * factor).ToList();
        // Json.NET is pinned by packages.lock.json; its reflection-based serializer
        // is usable in the process-owned runtime, without runtime downloads.
        report.Log(JsonConvert.SerializeObject(new { factor, values }));
        result.Numbers = new outputs.numbers.sample.NumbersT {
            Values = values, Label = received.Label
        };
        report.LineChart(new Series("calibrated", received.Label,
            Enumerable.Range(1, values.Count).Select(x => (double)x).ToArray(), values.ToArray()));
        return ValueTask.FromResult(result);
    }
}
