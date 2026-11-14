using Printf
include("ecology.jl")

# Similar fits over 21 years can mean different futures: a closed orbit, a spiral
# into a fixed point or a limit cycle. Each fit is run far beyond the data.
function main(inputs, report)
    f = Tsugumi.input(inputs, "fits")
    years = Int(inputs.params["years"])
    dt = inputs.params["dt"]
    rows = Dict{String,Any}[]
    ends = Dict{String,Any}[]
    swing = Dict{String,Float64}()
    for (model, m) in zip(MODELS, f["models"])
        p = Float64.(m["parameters"])
        per_year = 8
        Hs, Ls = simulate(model.rhs, p, p[end - 1], p[end], years; per_year, dt)
        # The size of the last cycles, in hare: a settled orbit keeps it, a spiral loses it.
        tail = max(1, length(Hs) - per_year * 30):length(Hs)
        swing[model.name] = maximum(Hs[tail]) - minimum(Hs[tail])
        report.log(string(@sprintf("%s: over the last 30 of %d simulated years the hare swings by %.2f, ending at H = %.2f, L = %.2f", model.name, years, swing[model.name], Hs[end], Ls[end])))
        for i in eachindex(Hs)
            push!(rows, Dict("H" => round(Hs[i]; digits=2), "L" => round(Ls[i]; digits=2), "t" => i, "model" => model.name,
                             "phase" => i <= per_year * 21 ? "The records' 21 years" : "The $(years - 21) years after"))
        end
    end
    names = [model.name for model in MODELS]
    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => "Similar fits can move quite differently ahead",
                        "subtitle" => "Phase diagrams of each model's best solution extended $(years) years ahead. Where there is no data, this is not a prophecy but the nature of the equations"),
        "usermeta" => Dict("swing" => swing),
        "data" => Dict("values" => rows),
        "facet" => Dict("field" => "model", "type" => "nominal", "sort" => names, "title" => nothing, "header" => Dict("labelFontSize" => 12)),
        "columns" => 3,
        "spec" => Dict("width" => 230, "height" => 240,
                       "mark" => Dict("type" => "line", "strokeWidth" => 1.3, "clip" => true),
                       "encoding" => Dict("x" => Dict("field" => "H", "type" => "quantitative", "title" => "Hare H (thousands of pelts)"),
                                          "y" => Dict("field" => "L", "type" => "quantitative", "title" => "Lynx L (thousands of pelts)"),
                                          "order" => Dict("field" => "t", "type" => "quantitative"),
                                          "color" => Dict("field" => "phase", "type" => "nominal", "title" => nothing,
                                                          "scale" => Dict("domain" => ["The records' 21 years", "The $(years - 21) years after"], "range" => ["#eb6834", "#2a78d6"]),
                                                          "legend" => Dict("orient" => "bottom"))))))

    return nothing
end
