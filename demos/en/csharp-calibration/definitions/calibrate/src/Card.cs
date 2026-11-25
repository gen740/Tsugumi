using System.Linq;
using System;
using Newtonsoft.Json;
using Tsugumi;

public static class Card {
    public static Output Run(Input inputs) {
        var result = new Output();
        var received = inputs.Numbers;
        double factor = Params.Current.Factor;
        var values = received.Values.Select(value => value * factor).ToList();
        // Json.NET is pinned by packages.lock.json; its reflection-based serializer
        // is usable in the process-owned runtime, without runtime downloads.
        Console.WriteLine(JsonConvert.SerializeObject(new { factor, values }));
        result.Numbers = new sample.NumbersT {
            Values = values, Label = received.Label
        };
        Effects.Send(new LineChart([new Series("calibrated", received.Label,
            Enumerable.Range(1, values.Count).Select(x => (double)x).ToArray(), values.ToArray())]));
        return result;
    }
}
