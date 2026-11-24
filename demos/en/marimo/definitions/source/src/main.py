from card import Input, Output, params
from notebook.Measurement import MeasurementT


def run(input: Input) -> Output:
    return Output(value=MeasurementT(source=params.measurement))
