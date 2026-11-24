from card import Input, Output, params
from Comparison import ComparisonT


def run(input: Input) -> Output:
    signal = params.signal
    threshold = params.threshold
    return Output(out=ComparisonT(signal=signal, threshold=threshold, detected=signal >= threshold))
