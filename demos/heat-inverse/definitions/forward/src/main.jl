using LinearAlgebra
using Printf
using Random

# The rod's centres of n equal cells on [0, 1], and the true initial temperature
# (made up): a smooth hump, a hot block and a fine ripple, on 20 degrees.
function true_profile(n)
    x = [(j - 0.5) / n for j in 1:n]
    hump(x) = 35 * exp(-((x - 0.2) / 0.06)^2)
    block(x) = 0.42 <= x <= 0.56 ? 25.0 : 0.0
    ripple(x) = 0.68 <= x <= 0.92 ? 12 * sin(2pi * (x - 0.68) / 0.08) : 0.0
    return x, [20 + hump(a) + block(a) + ripple(a) for a in x]
end

# The rod's finite differences with insulated ends form a symmetric matrix L, so
# A = exp(-alpha T L) = V diag(s) V', and s are its singular values, largest first.
function heat_modes(n, alpha, T)
    h = 1 / n
    diagonal = fill(2.0, n)
    diagonal[1] = diagonal[n] = 1.0
    F = eigen(SymTridiagonal(diagonal ./ h^2, fill(-1.0 / h^2, n - 1)))
    return F.vectors, exp.(-alpha * T .* max.(F.values, 0.0))
end

function main(ctx)
    s = input(ctx, "settings")
    n, alpha, T, noise = Int(s["cells"]), s["alpha"], s["final_time"], s["noise"]
    x, truth = true_profile(n)
    V, sigma = heat_modes(n, alpha, T)
    clean = V * (sigma .* (V' * truth))
    rng = Xoshiro(s["seed"])
    measured = clean .+ noise .* randn(rng, n)

    heat_change = (sum(clean) - sum(truth)) / sum(truth)
    abs(heat_change) < 1e-10 || error("insulated ends must keep the heat: relative change $heat_change")
    println(@sprintf("relative change of the total heat: %.2e", heat_change))

    output(ctx, "measurement", Dict("seed" => s["seed"], "alpha" => alpha, "final_time" => T, "noise" => noise,
                                    "x" => x, "truth" => truth, "measured" => measured))
    round3(v) = round(v; digits=3)
    lines = [Dict("x" => x[j], "u" => round3(series[j]), "line" => name)
             for (name, series) in (("初期の温度（真の値・架空）", truth), ("時刻 T の温度", clean)) for j in 1:n]
    points = [Dict("x" => x[j], "u" => round3(measured[j])) for j in 1:n]
    display_vegalite(ctx, Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("anchor" => "start", "text" => @sprintf("熱は細部から消える: T = %g の測定（ノイズ %g ℃）", T, noise),
                        "subtitle" => @sprintf("架空の棒。両端は断熱で、総熱量の相対変化は %.1e", heat_change)),
        "usermeta" => Dict("x" => x, "truth" => truth, "clean" => clean, "measured" => measured, "heat_change" => heat_change),
        "layer" => [
            Dict("data" => Dict("values" => lines), "mark" => Dict("type" => "line", "strokeWidth" => 2),
                 "encoding" => Dict("x" => Dict("field" => "x", "type" => "quantitative", "title" => "棒の位置 x"),
                                    "y" => Dict("field" => "u", "type" => "quantitative", "title" => "温度（℃）"),
                                    "color" => Dict("field" => "line", "type" => "nominal", "title" => nothing,
                                                    "scale" => Dict("domain" => ["初期の温度（真の値・架空）", "時刻 T の温度", "測定値"], "range" => ["#111111", "#eb6834", "#2a78d6"]),
                                                    "legend" => Dict("orient" => "top-right")))),
            Dict("data" => Dict("values" => points), "mark" => Dict("type" => "point", "filled" => true, "size" => 16, "color" => "#2a78d6"),
                 "encoding" => Dict("x" => Dict("field" => "x", "type" => "quantitative"), "y" => Dict("field" => "u", "type" => "quantitative"),
                                    "color" => Dict("datum" => "測定値"))),
        ]))
end
