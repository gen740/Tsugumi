function main(inputs, report)
    outputs = Dict{String,Any}()
    walk = Tsugumi.input(inputs, "number")
    half = UInt64(walk["value"]) ÷ 2
    outputs["next"] = Dict("value" => half, "path" => push!(UInt64.(walk["path"]), half))

    return outputs
end
