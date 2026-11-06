function main(ctx)
    walk = input(ctx, "number")
    n = UInt64(walk["value"])
    n > (typemax(UInt64) - 1) ÷ 3 && error("integer overflow: 3*$n+1 exceeds uint64")
    triple = 3n + 1
    output(ctx, "next", Dict("value" => triple, "path" => push!(UInt64.(walk["path"]), triple)))
end
