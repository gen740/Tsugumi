from tsugumi import Failure

UINT64_MAX = (1 << 64) - 1


def main(inputs):
    n = inputs.number.value
    result = 3 * n + 1
    if result > UINT64_MAX:
        raise Failure("output_invalid", f"integer overflow: 3*{n}+1 exceeds uint64")
    return {'next': {'value': result}}
