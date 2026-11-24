function main(inputs)
    outputs = Dict{String,Any}()
    walk = inputs.number
    n = UInt64(walk["value"])
    n > (typemax(UInt64) - 1) ÷ 3 && error("integer overflow: 3*$n+1 exceeds uint64")
    triple = 3n + 1
    outputs["next"] = Dict("value" => triple, "path" => push!(UInt64.(walk["path"]), triple))

    return outputs
end
