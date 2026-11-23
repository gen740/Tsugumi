from card import Input, Output
from inputs.results.Comparison import ComparisonT
from Summary import SummaryT


def run(input: Input) -> Output:
    items = input.require("results").items or []
    comparisons = [ComparisonT.InitFromPackedBuf(bytes(item.bytes)) for item in items]
    result = SummaryT(comparisons=len(comparisons), detections=sum(item.detected for item in comparisons))
    # TODO(open): Port the demo display.
    return Output(out=result)
