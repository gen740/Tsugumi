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

# The corner of the L-curve: where log(norm) against log(residual) bends most,
# by the finite-difference curvature along the sweep.
function corner(log_residual, log_norm)
    best, at = -Inf, 2
    for i in 2:length(log_norm)-1
        dx1, dy1 = log_residual[i] - log_residual[i - 1], log_norm[i] - log_norm[i - 1]
        dx2, dy2 = log_residual[i + 1] - log_residual[i], log_norm[i + 1] - log_norm[i]
        curvature = 2 * (dx1 * dy2 - dx2 * dy1) / (hypot(dx1, dy1) * hypot(dx2, dy2) * hypot(dx1 + dx2, dy1 + dy2) + 1e-300)
        if curvature > best
            best, at = curvature, i
        end
    end
    return at
end

# Tikhonov: minimize |A u - d|^2 + lambda^2 |u|^2, which damps each mode by the
# filter sigma / (sigma^2 + lambda^2). Three ways to choose lambda are compared.
function main(inputs, report)
    outputs = Dict{String,Any}()
    m = inputs.measurement
    x, truth, measured = Float64.(m["x"]), Float64.(m["truth"]), Float64.(m["measured"])
    n = length(x)
    noise = m["noise"]
    lambdas = exp10.(range(inputs.params["log10_lambda_min"], inputs.params["log10_lambda_max"]; length=Int(inputs.params["sweep"])))
    V, s = heat_modes(n, m["alpha"], m["final_time"])
    coefficients = V' * measured

    solve(l) = V * ((s ./ (s .^ 2 .+ l^2)) .* coefficients)
    residual(u) = norm(V * (s .* (V' * u)) - measured)
    rms(v) = sqrt(sum(abs2, v) / length(v))
    solutions = [solve(l) for l in lambdas]
    residuals = residual.(solutions)
    norms = norm.(solutions)
    errors = [rms(u - truth) for u in solutions]

    # Discrepancy principle: the largest lambda that fits the data down to the noise.
    target = noise * sqrt(n)
    candidates = findall(<=(target), residuals)
    discrepancy = isempty(candidates) ? 1 : maximum(candidates)
    lcurve = corner(log.(residuals), log.(norms))
    oracle = argmin(errors)

    describe_choice(name, i) = report.log(string(@sprintf("%s: lambda %.3g, RMS error %.3f, residual %.3f", name, lambdas[i], errors[i], residuals[i])))
    describe_choice("discrepancy", discrepancy)
    describe_choice("L-curve", lcurve)
    describe_choice("best against the truth", oracle)
    outputs["choice"] = Dict("discrepancy" => lambdas[discrepancy], "lcurve" => lambdas[lcurve], "oracle" => lambdas[oracle],
                               "error_discrepancy" => errors[discrepancy], "error_lcurve" => errors[lcurve], "error_oracle" => errors[oracle])

    sweep = [Dict("lambda" => lambdas[i], "residual" => residuals[i], "norm" => norms[i], "error" => errors[i]) for i in eachindex(lambdas)]
    marks = [Dict("lambda" => lambdas[i], "residual" => residuals[i], "norm" => norms[i], "error" => errors[i], "method" => name)
             for (name, i) in (("Discrepancy principle", discrepancy), ("Corner of the L", lcurve), ("Closest to the truth", oracle))]
    method = Dict("field" => "method", "type" => "nominal", "title" => "How λ is chosen",
                  "scale" => Dict("domain" => ["Discrepancy principle", "Corner of the L", "Closest to the truth"], "range" => ["#2a78d6", "#eb6834", "#1baf7a"]))
    mark_point = Dict("type" => "point", "filled" => true, "size" => 110, "stroke" => "white", "strokeWidth" => 1.5, "opacity" => 1)
    lambda_axis = Dict("field" => "lambda", "type" => "quantitative", "title" => "Strength of regularisation λ", "scale" => Dict("type" => "log"), "axis" => Dict("format" => ".0e", "tickCount" => 5))
    log_x = Dict("field" => "residual", "type" => "quantitative", "title" => "Residual ‖Au − d‖", "scale" => Dict("type" => "log", "nice" => false, "zero" => false), "axis" => Dict("format" => "~g", "tickCount" => 5, "grid" => false))
    log_y = Dict("field" => "norm", "type" => "quantitative", "title" => "Size of the solution ‖u‖", "scale" => Dict("type" => "log", "nice" => false, "zero" => false), "axis" => Dict("format" => "~g", "tickCount" => 5, "grid" => false))
    y_error = Dict("field" => "error", "type" => "quantitative", "title" => "RMS error against the truth (°C)", "scale" => Dict("type" => "log", "nice" => false), "axis" => Dict("format" => "~g", "tickCount" => 5, "grid" => false))

    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => @sprintf("The trade-off of λ: by the discrepancy principle λ = %.2g, error %.2f °C", lambdas[discrepancy], errors[discrepancy]),
                        "subtitle" => @sprintf("At the corner of the L, λ = %.2g (error %.2f °C). Knowing the truth, λ = %.2g (error %.2f °C)", lambdas[lcurve], errors[lcurve], lambdas[oracle], errors[oracle])),
        "usermeta" => Dict("lambdas" => lambdas, "errors" => errors, "residuals" => residuals, "norms" => norms,
                           "discrepancy" => lambdas[discrepancy], "lcurve" => lambdas[lcurve], "oracle" => lambdas[oracle]),
        "hconcat" => [
            Dict("width" => 290, "height" => 240,
                 "title" => Dict("text" => "L-curve", "fontSize" => 12),
                 "layer" => [
                     Dict("data" => Dict("values" => sweep), "mark" => Dict("type" => "line", "point" => Dict("size" => 12), "color" => "#8a8880"),
                          "encoding" => Dict("x" => log_x, "y" => log_y, "order" => Dict("field" => "lambda", "type" => "quantitative"))),
                     Dict("data" => Dict("values" => marks), "mark" => mark_point, "encoding" => Dict("x" => log_x, "y" => log_y, "color" => method)),
                 ]),
            Dict("width" => 290, "height" => 240,
                 "title" => Dict("text" => "λ and error", "fontSize" => 12),
                 "layer" => [
                     Dict("data" => Dict("values" => sweep), "mark" => Dict("type" => "line", "color" => "#8a8880", "point" => Dict("size" => 12)),
                          "encoding" => Dict("x" => lambda_axis, "y" => y_error)),
                     Dict("data" => Dict("values" => marks), "mark" => mark_point, "encoding" => Dict("x" => lambda_axis, "y" => y_error, "color" => method)),
                 ]),
        ]))

    return outputs
end
