def main(inputs, report):
    outputs = {}
    value = inputs.params.value
    outputs.update({'out': {'value': value}})
    report.scalar(value, 'Python')
    return outputs
