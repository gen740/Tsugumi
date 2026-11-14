using Printf
include("ecology.jl")

# Fits each model to the record up to `fit_until` and scores it. The noise level
# is estimated too, so it counts as one more parameter in AIC and BIC.
function main(inputs, report)
    outputs = Dict{String,Any}()
    m = Tsugumi.input(inputs, "pelts")
    years = Int.(m["years"])
    hare, lynx = Float64.(m["hare"]), Float64.(m["lynx"])
    until = Int(inputs.params["fit_until"])
    restarts = Int(inputs.params["restarts"])
    dt = inputs.params["dt"]
    used = findall(<=(until), years)
    rng = Xoshiro(inputs.params["seed"])
    n = 2 * length(used)

    results = Dict{String,Any}[]
    curves = Dict{String,Any}[]
    span = years[end] - years[1]
    for model in MODELS
        fit = fit_model(model, hare[used], lynx[used], restarts, rng, dt)
        p = fit.parameters
        k = length(p) + 1
        aic = n * log(fit.error / n) + 2k
        aicc = aic + 2k * (k + 1) / (n - k - 1)
        bic = n * log(fit.error / n) + k * log(n)
        Hs, Ls = simulate(model.rhs, p, p[end - 1], p[end], span; per_year=4, dt)
        report.log(string(@sprintf("%s: error %.4f, k = %d, AIC %.2f, AICc %.2f, BIC %.2f, %d of %d starts agree", model.name, fit.error, k, aic, aicc, bic, fit.agreeing, restarts)))
        report.log(string("  ", join((@sprintf("%s = %.4g", name, v) for (name, v) in zip(vcat(model.names, ["H0", "L0"]), p)), ", ")))
        push!(results, Dict("name" => model.name, "parameter_names" => vcat(model.names, ["H0", "L0"]), "parameters" => p,
                            "sse" => fit.error, "n" => n, "k" => k, "aic" => aic, "aicc" => aicc, "bic" => bic,
                            "rmse_log" => sqrt(fit.error / n), "agreeing" => fit.agreeing, "restarts" => restarts))
        for i in eachindex(Hs)
            year = years[1] + (i - 1) / 4
            push!(curves, Dict("year" => year, "pelts" => round(Hs[i]; digits=2), "species" => "Hare", "model" => model.name))
            push!(curves, Dict("year" => year, "pelts" => round(Ls[i]; digits=2), "species" => "Lynx", "model" => model.name))
        end
    end
    outputs["fits"] = Dict("fit_until" => until, "models" => results)

    observed = [Dict("year" => years[i], "pelts" => series[i], "species" => name)
                for (name, series) in (("Hare", hare), ("Lynx", lynx)) for i in eachindex(years)]
    names = [model.name for model in MODELS]
    colors = Dict("domain" => names, "range" => ["#2a78d6", "#1baf7a", "#eb6834"])
    dashes = Dict("domain" => names, "range" => [[1, 0], [1, 0], [5, 4]]) # the fits can coincide
    holling = results[3]["parameters"]
    panel(species, title)= Dict("width" => 640, "height" => 140,
        "layer" => [
            Dict("data" => Dict("values" => filter(r -> r["species"] == species, curves)), "mark" => Dict("type" => "line", "strokeWidth" => 1.8),
                 "encoding" => Dict("x" => Dict("field" => "year", "type" => "quantitative", "title" => "Year", "scale" => Dict("zero" => false, "nice" => false), "axis" => Dict("format" => "d")),
                                    "y" => Dict("field" => "pelts", "type" => "quantitative", "title" => title),
                                    "color" => Dict("field" => "model", "type" => "nominal", "title" => "Model", "scale" => colors),
                                    "strokeDash" => Dict("field" => "model", "type" => "nominal", "title" => "Model", "scale" => dashes))),
            Dict("data" => Dict("values" => filter(r -> r["species"] == species, observed)), "mark" => Dict("type" => "point", "filled" => true, "size" => 34, "color" => "#111111"),
                 "encoding" => Dict("x" => Dict("field" => "year", "type" => "quantitative"), "y" => Dict("field" => "pelts", "type" => "quantitative"))),
        ])
    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => "Three models fitted to the records of $(years[1])–$(until)",
                        "subtitle" => [
                            "Black dots are the fur records (thousands of pelts). Lines are each model's best solution (minimising the error of the logs, from $(restarts) starting points)",
                            @sprintf("Holling type II's handling time h = %.2g, carrying capacity K = %.0f. With h near 0 and K large, the added terms are barely used", holling[6], holling[5]),
                        ]),
        "usermeta" => Dict("fits" => results),
        "vconcat" => [panel("Hare", "Hare (thousands of pelts)"), panel("Lynx", "Lynx (thousands of pelts)")]))

    return outputs
end
