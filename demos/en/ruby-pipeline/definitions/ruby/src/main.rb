def main(inputs)
  outputs = {}
  received = inputs.number.fetch('value')
  factor = Tsugumi.params.fetch('factor')
  result = received * factor
  outputs['out'] = {value: result}
  puts "Ruby: #{received} -> #{result}"
  Tsugumi.send(Tsugumi::Table.new(['input', 'factor', 'result'], [[received, factor, result]]))

  outputs
end
