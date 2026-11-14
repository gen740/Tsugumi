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

# Dividing by the singular values: the noise (or even round-off) along the
# smallest ones is multiplied by 1 / sigma. The values are limited only so that
# they stay representable.
function main(inputs, report)
    m = inputs.measurement
    x, truth, measured = Float64.(m["x"]), Float64.(m["truth"]), Float64.(m["measured"])
    n = length(x)
    V, s = heat_modes(n, m["alpha"], m["final_time"])
    gain = 1 ./ max.(s, 1e-290)
    invert(d) = clamp.(V * (gain .* (V' * d)), -1e300, 1e300)
    from_measured = invert(measured)
    from_clean = invert(V * (s .* (V' * truth)))
    peak = maximum(abs, from_measured)
    peak_clean = maximum(abs, from_clean)
    report.log(string(@sprintf("largest |u0| from the measured data %.3g, from noise-free data (round-off only) %.3g; truth %.3g", peak, peak_clean, maximum(abs, truth))))

    rows = [Dict("x" => x[j], "u" => series[j], "line" => name)
            for (name, series) in (("素朴な逆解（測定から）", from_measured), ("素朴な逆解（ノイズなし）", from_clean), ("真の初期温度", truth)) for j in 1:n]
    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("anchor" => "start", "text" => @sprintf("素朴に逆を解くと発散する: 最大 %.1e ℃", peak),
                        "subtitle" => @sprintf("A⁻¹ d をそのまま計算した結果（真の値の最大は %.0f ℃、縦軸は対称対数）。ノイズなしでも丸め誤差だけで %.1e ℃", maximum(truth), peak_clean)),
        "usermeta" => Dict("peak" => peak, "peak_clean" => peak_clean, "smallest_singular_value" => s[end]),
        "data" => Dict("values" => rows),
        "mark" => Dict("type" => "line", "strokeWidth" => 1.4),
        "encoding" => Dict("x" => Dict("field" => "x", "type" => "quantitative", "title" => "棒の位置 x"),
                           "y" => Dict("field" => "u", "type" => "quantitative", "title" => "推定した初期温度（℃）",
                                       "scale" => Dict("type" => "symlog", "constant" => 100.0),
                                       "axis" => Dict("format" => ".0e", "values" => [-1e30, -1e20, -1e10, 0, 1e10, 1e20, 1e30])),
                           "color" => Dict("field" => "line", "type" => "nominal", "title" => nothing,
                                           "scale" => Dict("domain" => ["素朴な逆解（測定から）", "素朴な逆解（ノイズなし）", "真の初期温度"], "range" => ["#2a78d6", "#eda100", "#111111"]),
                                           "legend" => Dict("orient" => "bottom")))))

    return nothing
end
