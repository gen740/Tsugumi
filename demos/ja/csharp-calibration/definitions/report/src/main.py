from pathlib import Path


def main(inputs, report, run):
    readings = inputs.numbers
    values = [readings.values[i] for i in range(len(readings.values))]
    document = Path(run.output_directory) / "calibrated.csv"
    document.write_text("sample,value\n" + "".join(f"{i},{value:.12g}\n" for i, value in enumerate(values, 1)))
    report.table(["samples", "sum", "mean"], [[str(len(values)), f"{sum(values):.12g}", f"{sum(values) / len(values):.12g}"]])
    name = str(document)
    return {'report': {'value': name}}
