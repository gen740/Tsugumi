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
    send(Markdown("Sequence: " * join(path, " ")))
    n == 1 && (outputs["sequence"] = Dict("values" => path))

    return outputs
end
