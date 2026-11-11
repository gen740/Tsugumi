function main(ctx)
    walk = input(ctx, "number")
    half = UInt64(walk["value"]) ÷ 2
    output(ctx, "next", Dict("value" => half, "path" => push!(UInt64.(walk["path"]), half)))
end
