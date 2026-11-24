# Initial values start a sequence. Feedback carries the sequence so far.
function arrival(inputs)
    value = inputs.arrival["value"]
    if haskey(value, "seed")
        n = UInt64(value["seed"])
        return n, UInt64[n]
    end
    return UInt64(value["value"]), UInt64.(value["path"])
end

function main(inputs)
    outputs = Dict{String,Any}()
    n, path = arrival(inputs)
    n == 0 && error("Collatz is defined for positive integers")
    n == 1 && return outputs
    outputs["value"] = Dict("value" => n, "path" => path)

    return outputs
end
