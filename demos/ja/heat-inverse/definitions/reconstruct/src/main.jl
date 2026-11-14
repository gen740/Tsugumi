using LinearAlgebra
using Printf

# The rod's finite differences with insulated ends form a symmetric matrix L, so
# A = exp(-alpha T L) = V diag(s) V', and s are its singular values, largest first.
function heat_modes(n, alpha, T)
    h = 1 / n
    diagonal = fill(2.0, n)
    diagonal[1] = diagonal[n] = 1.0
    F = eigen(SymTridiagonal(diagonal ./ h^2, fill(-1.0 / h^2, n - 1)))
    return F.vectors, exp.(-alpha * T .* max.(F.values, 0.0))
end

# The reconstruction at the chosen lambda, with one too small and one too large
# beside it. `lambda` = 0 takes the discrepancy principle's choice.
function main(inputs, report)
    m = Tsugumi.input(inputs, "measurement")
    choice = Tsugumi.input(inputs, "choice")
    x, truth, measured = Float64.(m["x"]), Float64.(m["truth"]), Float64.(m["measured"])
    n = length(x)
    chosen = inputs.params["lambda"] > 0 ? inputs.params["lambda"] : choice["discrepancy"]
    V, s = heat_modes(n, m["alpha"], m["final_time"])
    coefficients = V' * measured
    solve(l) = V * ((s ./ (s .^ 2 .+ l^2)) .* coefficients)
    rms(v) = sqrt(sum(abs2, v) / length(v))

    factor = inputs.params["too_far_factor"]
    cases = (("小さすぎる λ", chosen / factor), ("選んだ λ", chosen), ("大きすぎる λ", chosen * factor))
    solutions = [solve(l) for (_, l) in cases]
    errors = [rms(u - truth) for u in solutions]
    names = [@sprintf("%s = %.2g（誤差 %.1f ℃）", cases[i][1], cases[i][2], errors[i]) for i in 1:3]
    foreach(println, names)

    panels = [Dict("x" => x[j], "u" => round(solutions[i][j]; digits=3), "panel" => names[i]) for i in 1:3 for j in 1:n]
    # A wild solution would hide the truth: the vertical axis follows the truth.
    low, high = minimum(truth) - 25, maximum(truth) + 25
    shown = [merge(p, Dict("u" => clamp(p["u"], low, high))) for p in panels]
    reference = [Dict("x" => x[j], "truth" => truth[j], "panel" => names[i]) for i in 1:3 for j in 1:n]
    yscale = Dict("domain" => [low, high], "nice" => false)
    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => @sprintf("λ を変えた復元: 選んだ λ = %.2g で誤差 %.2f ℃", chosen, errors[2]),
                        "subtitle" => "黒: 真の初期温度（架空）。青: 測定からの復元。小さすぎると振動し、大きすぎると細部がなまる（縦軸の外は枠で切った）"),
        "usermeta" => Dict("lambda" => chosen, "lambdas" => [c[2] for c in cases], "errors" => errors, "solution" => solutions[2]),
        "data" => Dict("values" => vcat(shown, reference)),
        "facet" => Dict("field" => "panel", "type" => "nominal", "sort" => names, "title" => nothing, "header" => Dict("labelFontSize" => 11)),
        "columns" => 3,
        "spec" => Dict("width" => 235, "height" => 230,
                       "layer" => [
                           Dict("transform" => [Dict("filter" => "isValid(datum.truth)")], "mark" => Dict("type" => "line", "color" => "#111111", "strokeWidth" => 1.5),
                                "encoding" => Dict("x" => Dict("field" => "x", "type" => "quantitative", "title" => "棒の位置 x"),
                                                   "y" => Dict("field" => "truth", "type" => "quantitative", "title" => "初期温度（℃）", "scale" => yscale))),
                           Dict("transform" => [Dict("filter" => "isValid(datum.u)")], "mark" => Dict("type" => "line", "color" => "#2a78d6", "strokeWidth" => 1.6),
                                "encoding" => Dict("x" => Dict("field" => "x", "type" => "quantitative"),
                                                   "y" => Dict("field" => "u", "type" => "quantitative", "scale" => yscale))),
                       ])))

    return nothing
end
