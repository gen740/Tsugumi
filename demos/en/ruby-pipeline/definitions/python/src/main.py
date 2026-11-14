import tsugumi

def main(inputs, report):
    result_ports = {}
    value = inputs.params.value
    _number_value = {}
    _number_value['value'] = value
    result_ports.update({'out': _number_value})
    report.scalar(value, 'Python')
    return result_ports
