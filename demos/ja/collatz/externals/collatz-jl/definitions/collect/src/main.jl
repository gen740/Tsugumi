# The loop delivers a merge union tagged with the arrival port: `init` carries a Number, `half` and `triple`
# a Walk (the value and every value before it, so that no Card has to remember anything between calls).
# Julia's UInt64 holds every value of the message exactly.
function arrival(ctx)
    m = input(ctx, "arrival")
    n = UInt64(m["value"]["value"])
    return n, m["value_type"] == "init" ? UInt64[n] : UInt64.(m["value"]["path"])
end

function main(ctx)
    n, path = arrival(ctx)
    display_text(ctx, "$(length(path)) values: " * join(path, " "))
    n == 1 && output(ctx, "sequence", Dict("values" => path))
end
