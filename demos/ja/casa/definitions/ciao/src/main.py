from card import Input, Output, params
from casa.Ciao import CiaoT


def run(input: Input) -> Output:
    return Output(tool=CiaoT(version=params.version))
