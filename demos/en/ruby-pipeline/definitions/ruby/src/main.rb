def on_run_start(inputs)
  @runs = (@runs || 0) + 1
  nil
end

def main(inputs, report)
  outputs = {}
  received = inputs.number.fetch('value')
  result = received * inputs.params.fetch('factor')
  outputs['out'] = {value: result}
  report.log("Ruby run #{@runs}: #{received} -> #{result}")
  report.table(['input', 'factor', 'result'], [[received, inputs.params['factor'], result]])

  outputs
end
