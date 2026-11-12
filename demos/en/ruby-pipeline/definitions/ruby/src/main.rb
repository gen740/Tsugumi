def on_run_start(ctx)
  @runs = (@runs || 0) + 1
end

def main(ctx)
  input = ctx.input('number').fetch('value')
  result = input * ctx.params.fetch('factor')
  ctx.output('out', value: result)
  ctx.log("Ruby run #{@runs}: #{input} -> #{result}")
  ctx.display_table(['input', 'factor', 'result'], [[input, ctx.params['factor'], result]])
end
