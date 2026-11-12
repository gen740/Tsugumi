from pathlib import Path
from inputs.numbers.sample.Numbers import Numbers
import outputs.report.sample.Location as Location


def main(ctx):
    readings = ctx.input("numbers", Numbers)
    values = [readings.Values(i) for i in range(readings.ValuesLength())]
    report = Path(ctx.output_directory) / "calibrated.csv"
    report.write_text("sample,value\n" + "".join(f"{i},{value:.12g}\n" for i, value in enumerate(values, 1)))
    ctx.display_table(["samples", "sum", "mean"], [[str(len(values)), f"{sum(values):.12g}", f"{sum(values) / len(values):.12g}"]])
    builder = ctx.builder()
    name = builder.CreateString(str(report))
    Location.Start(builder)
    Location.AddValue(builder, name)
    ctx.output("report", builder, Location.End(builder))
