using Printf
include("ecology.jl")

# The test AIC cannot give: fit only the early years, then compare each model's
# simulation with the years it has not seen.
function main(inputs, report)
    outputs = Dict{String,Any}()
    m = inputs.pelts
    years = Int.(m["years"])
    hare, lynx = Float64.(m["hare"]), Float64.(m["lynx"])
    until = Int(inputs.params["fit_until"])
    restarts = Int(inputs.params["restarts"])
    dt = inputs.params["dt"]
    used = findall(<=(until), years)
    held = findall(>(until), years)
    isempty(held) && error("fit_until must leave at least one year out")
    rng = Xoshiro(inputs.params["seed"])

    span = years[end] - years[1]
    results = Dict{String,Any}[]
    curves = Dict{String,Any}[]
    for model in MODELS
        fit = fit_model(model, hare[used], lynx[used], restarts, rng, dt)
        p = fit.parameters
        Hs, Ls = simulate(model.rhs, p, p[end - 1], p[end], span; dt)
        errors = vcat(log.(hare[held]) .- log.(Hs[held]), log.(lynx[held]) .- log.(Ls[held]))
        rmse = sqrt(sum(abs2, errors) / length(errors))
        in_sample = sqrt(fit.error / (2 * length(used)))
        report.log(string(@sprintf("%s: RMSE of log counts %.3f on the fitted years, %.3f on the held-out years", model.name, in_sample, rmse)))
        push!(results, Dict("name" => model.name, "rmse_fit" => in_sample, "rmse_heldout" => rmse, "parameters" => p,
                            "hare" => Hs, "lynx" => Ls))
        fine_H, fine_L = simulate(model.rhs, p, p[end - 1], p[end], span; per_year=4, dt)
        for i in eachindex(fine_H)
            year = years[1] + (i - 1) / 4
            push!(curves, Dict("year" => year, "pelts" => round(fine_H[i]; digits=2), "species" => "ノウサギ", "model" => model.name))
            push!(curves, Dict("year" => year, "pelts" => round(fine_L[i]; digits=2), "species" => "オオヤマネコ", "model" => model.name))
        end
    end
    outputs["forecast"] = Dict("fit_until" => until, "last_year" => years[end],
                                 "names" => [r["name"] for r in results], "rmse_fit" => [r["rmse_fit"] for r in results], "rmse_heldout" => [r["rmse_heldout"] for r in results])

    observed = [Dict("year" => years[i], "pelts" => series[i], "species" => name)
                for (name, series) in (("ノウサギ", hare), ("オオヤマネコ", lynx)) for i in eachindex(years)]
    names = [model.name for model in MODELS]
    colors = Dict("domain" => names, "range" => ["#2a78d6", "#1baf7a", "#eb6834"])
    dashes = Dict("domain" => names, "range" => [[1, 0], [1, 0], [5, 4]]) # the fits can coincide
    best = results[argmin([r["rmse_heldout"] for r in results])]
    # Very wild forecasts should not flatten the chart.
    top(values) = 1.25 * maximum(values)
    panel(species, title, values) = Dict("width" => 640, "height" => 140,
        "layer" => [
            Dict("data" => Dict("values" => [Dict("from" => until + 0.5, "to" => years[end])]), "mark" => Dict("type" => "rect", "color" => "#8a8880", "opacity" => 0.14),
                 "encoding" => Dict("x" => Dict("field" => "from", "type" => "quantitative"), "x2" => Dict("field" => "to"))),
            Dict("data" => Dict("values" => filter(r -> r["species"] == species, curves)), "mark" => Dict("type" => "line", "strokeWidth" => 1.8, "clip" => true),
                 "encoding" => Dict("x" => Dict("field" => "year", "type" => "quantitative", "title" => "年", "scale" => Dict("zero" => false, "nice" => false, "domain" => [years[1], years[end]]), "axis" => Dict("format" => "d")),
                                    "y" => Dict("field" => "pelts", "type" => "quantitative", "title" => title, "scale" => Dict("domain" => [0, top(values)], "nice" => false)),
                                    "color" => Dict("field" => "model", "type" => "nominal", "title" => "モデル", "scale" => colors),
                                    "strokeDash" => Dict("field" => "model", "type" => "nominal", "title" => "モデル", "scale" => dashes))),
            Dict("data" => Dict("values" => filter(r -> r["species"] == species, observed)), "mark" => Dict("type" => "point", "filled" => true, "size" => 34, "color" => "#111111"),
                 "encoding" => Dict("x" => Dict("field" => "year", "type" => "quantitative"), "y" => Dict("field" => "pelts", "type" => "quantitative"))),
        ])
    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => @sprintf("%d 年までで当てはめ、灰色の %d〜%d 年を予測する", until, years[held[1]], years[end]),
                        "subtitle" => ["対数の RMSE（予測 / 当てはめ）",
                                       join((@sprintf("%s %.2f / %.2f", r["name"], r["rmse_heldout"], r["rmse_fit"]) for r in results), "、")]),
        "usermeta" => Dict("results" => results, "fit_until" => until, "best" => best["name"]),
        "vconcat" => [panel("ノウサギ", "ノウサギ（千枚）", hare), panel("オオヤマネコ", "オオヤマネコ（千枚）", lynx)]))

    return outputs
end
