using System.Linq;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Tsugumi;

public sealed class Card : ICard {
    public ValueTask RunAsync(Context context) {
        var input = context.Inputs.Numbers;
        double factor = context.Parameters.Factor
            ?? throw new Failure("validation_failed", "factor is required");
        var values = input.Values.Select(value => value * factor).ToList();
        // Json.NET is pinned in nuget-deps.json; its reflection-based serializer
        // is usable in the process-owned runtime, without runtime downloads.
        context.Log(JsonConvert.SerializeObject(new { factor, values }));
        context.Outputs.Numbers(new outputs.numbers.sample.NumbersT {
            Values = values, Label = input.Label
        });
        context.DisplayLineChart(new Series("calibrated", input.Label,
            Enumerable.Range(1, values.Count).Select(x => (double)x).ToArray(), values.ToArray()));
        return ValueTask.CompletedTask;
    }
}
