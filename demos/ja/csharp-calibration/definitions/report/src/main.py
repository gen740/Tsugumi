import tsugumi
from pathlib import Path
from inputs.numbers.sample.Numbers import Numbers


def main(inputs, report, run):
    readings = inputs.input("numbers", Numbers)
    values = [readings.Values(i) for i in range(readings.ValuesLength())]
    document = Path(run.output_directory) / "calibrated.csv"
    document.write_text("sample,value\n" + "".join(f"{i},{value:.12g}\n" for i, value in enumerate(values, 1)))
    report.table(["samples", "sum", "mean"], [[str(len(values)), f"{sum(values):.12g}", f"{sum(values) / len(values):.12g}"]])
    name = str(document)
    _Location_value = {}
    _Location_value['value'] = name
    return {'report': _Location_value}
