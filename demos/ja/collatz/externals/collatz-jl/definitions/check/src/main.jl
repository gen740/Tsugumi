# The loop delivers a merge union tagged with the arrival port: `init` carries a Number, `half` and `triple`
# a Walk (the value and every value before it, so that no Card has to remember anything between calls).
# Julia's UInt64 holds every value of the message exactly.
function arrival(ctx)
    m = input(ctx, "arrival")
    n = UInt64(m["value"]["value"])
    return n, m["value_type"] == "init" ? UInt64[n] : UInt64.(m["value"]["path"])
end

# Emits nothing at 1 (the loop ends); otherwise the walk so far and the parity of its last value.
function main(ctx)
    n, path = arrival(ctx)
    n == 0 && error("Collatz is defined for positive integers")
    n == 1 && return
    output(ctx, "value", Dict("value" => n, "path" => path))
    output(ctx, "even", Dict("value" => iseven(n)))
end
