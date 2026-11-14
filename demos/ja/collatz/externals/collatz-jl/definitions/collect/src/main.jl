# The loop delivers a merge union tagged with the arrival port: `init` carries a Number, `half` and `triple`
# a Walk (the value and every value before it, so that no Card has to remember anything between calls).
# Julia's UInt64 holds every value of the message exactly.
function arrival(inputs, report)
    m = Tsugumi.input(inputs, "arrival")
    n = UInt64(m["value"]["value"])
    return n, m["value_type"] == "init" ? UInt64[n] : UInt64.(m["value"]["path"])
end

function main(inputs, report)
    outputs = Dict{String,Any}()
    n, path = arrival(inputs, report)
    report.text("$(length(path)) values: " * join(path, " "))
    n == 1 && outputs["sequence"] = Dict("values" => path)

    return outputs
end
