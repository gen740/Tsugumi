using LinearAlgebra
using Printf
using Random

# The rod's finite differences with insulated ends form a symmetric matrix L, so
# A = exp(-alpha T L) = V diag(s) V', and s are its singular values, largest first.
function heat_modes(n, alpha, T)
    h = 1 / n
    diagonal = fill(2.0, n)
    diagonal[1] = diagonal[n] = 1.0
    F = eigen(SymTridiagonal(diagonal ./ h^2, fill(-1.0 / h^2, n - 1)))
    return F.vectors, exp.(-alpha * T .* max.(F.values, 0.0))
end

# The same rod and the same noise, measured later and later: each time lambda is
# set by the discrepancy principle. Waiting loses detail that no method brings back.
function main(inputs, report)
    m = inputs.measurement
    x, truth = Float64.(m["x"]), Float64.(m["truth"])
    n = length(x)
    noise = m["noise"]
    times = sort(Float64.(inputs.params["times"]))
    lambdas = exp10.(range(-8, 2; length=121))
    rng = Xoshiro(m["seed"] + 1)
    rms(v) = sqrt(sum(abs2, v) / length(v))

    labels = String[]
    errors = Float64[]
    chosen = Float64[]
    modes = Int[]
    rows = Dict{String,Any}[]
    for T in times
        V, s = heat_modes(n, m["alpha"], T)
        clean = V * (s .* (V' * truth))
        measured = clean .+ noise .* randn(rng, n)
        coefficients = V' * measured
        solve(l) = V * ((s ./ (s .^ 2 .+ l^2)) .* coefficients)
        fits = findall(l -> norm(V * (s .* (V' * solve(l))) - measured) <= noise * sqrt(n), lambdas)
        l = isempty(fits) ? lambdas[1] : lambdas[maximum(fits)]
        u = solve(l)
        push!(errors, rms(u - truth))
        push!(chosen, l)
        push!(modes, count(abs.(V' * clean) .> noise))
        label = @sprintf("T = %g (error %.1f °C)", T, errors[end])
        push!(labels, label)
        append!(rows, [Dict("x" => x[j], "u" => round(u[j]; digits=3), "panel" => label) for j in 1:n])
        append!(rows, [Dict("x" => x[j], "truth" => truth[j], "panel" => label) for j in 1:n])
        report.log(string(@sprintf("T = %g: lambda %.3g, RMS error %.3f, %d modes above the noise", T, l, errors[end], modes[end])))
    end
    summary = [Dict("T" => times[i], "error" => errors[i], "modes" => modes[i]) for i in eachindex(times)]

    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => "The later you measure, the more detail is lost",
                        "subtitle" => @sprintf("The same rod measured with noise %g °C. Black: truth (made up); blue: recovered. Below: the recovery's error, and the number of modes left above the noise", noise)),
        "usermeta" => Dict("times" => times, "errors" => errors, "lambdas" => chosen, "modes" => modes),
        "vconcat" => [
            Dict("data" => Dict("values" => rows),
                 "facet" => Dict("field" => "panel", "type" => "nominal", "sort" => labels, "title" => nothing, "header" => Dict("labelFontSize" => 11)),
                 "columns" => 3,
                 "spec" => Dict("width" => 235, "height" => 130,
                                "layer" => [
                                    Dict("transform" => [Dict("filter" => "isValid(datum.truth)")], "mark" => Dict("type" => "line", "color" => "#111111", "strokeWidth" => 1.4),
                                         "encoding" => Dict("x" => Dict("field" => "x", "type" => "quantitative", "title" => "Position along the rod x"),
                                                            "y" => Dict("field" => "truth", "type" => "quantitative", "title" => "Initial temperature (°C)"))),
                                    Dict("transform" => [Dict("filter" => "isValid(datum.u)")], "mark" => Dict("type" => "line", "color" => "#2a78d6", "strokeWidth" => 1.6),
                                         "encoding" => Dict("x" => Dict("field" => "x", "type" => "quantitative"), "y" => Dict("field" => "u", "type" => "quantitative"))),
                                ])),
            Dict("hconcat" => [
                Dict("width" => 350, "height" => 110, "data" => Dict("values" => summary), "mark" => Dict("type" => "line", "point" => true, "color" => "#eb6834", "strokeWidth" => 2),
                     "encoding" => Dict("x" => Dict("field" => "T", "type" => "quantitative", "title" => "Measurement time T", "scale" => Dict("type" => "log"), "axis" => Dict("format" => "~g", "tickCount" => 4, "grid" => false)),
                                        "y" => Dict("field" => "error", "type" => "quantitative", "title" => "RMS error (°C)"))),
                Dict("width" => 350, "height" => 110, "data" => Dict("values" => summary), "mark" => Dict("type" => "line", "point" => true, "color" => "#2a78d6", "strokeWidth" => 2),
                     "encoding" => Dict("x" => Dict("field" => "T", "type" => "quantitative", "title" => "Measurement time T", "scale" => Dict("type" => "log"), "axis" => Dict("format" => "~g", "tickCount" => 4, "grid" => false)),
                                        "y" => Dict("field" => "modes", "type" => "quantitative", "title" => "Modes left"))),
            ]),
        ]))

    return nothing
end
