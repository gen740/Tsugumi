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

# Why it is ill-posed: the singular values fall steeply, and all modes whose
# signal is under the noise are lost to any inversion.
function main(ctx)
    m = input(ctx, "measurement")
    x, truth, measured = Float64.(m["x"]), Float64.(m["truth"]), Float64.(m["measured"])
    alpha, T, noise = m["alpha"], m["final_time"], m["noise"]
    n = length(x)
    factors = Float64.(ctx.params["time_factors"])

    smallest = 1e-30 # below this the chart stops
    decay = Dict{String,Any}[]
    labels = String[]
    for f in factors
        label = @sprintf("T = %g", f * T)
        push!(labels, label)
        _, s = heat_modes(n, alpha, f * T)
        for k in 1:n
            s[k] > smallest && push!(decay, Dict("k" => k, "sigma" => s[k], "time" => label))
        end
    end

    # The Picard picture at this T: the data along each mode, against the signal.
    V, s = heat_modes(n, alpha, T)
    signal = abs.(s .* (V' * truth))
    seen = abs.(V' * measured)
    resolved = noise > 0 ? count(signal .> noise) : n
    println(@sprintf("modes with a signal above the noise at T = %g: %d of %d; sigma_1 = %.3g, sigma_%d = %.3g", T, resolved, n, s[1], n, s[n]))
    floor_value = 1e-6
    picard = [Dict("k" => k, "v" => v[k], "series" => name)
              for (name, v) in (("初期温度が残した信号", signal), ("測定に含まれる成分", seen)) for k in 1:n if v[k] >= floor_value]

    display_vegalite(ctx, Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => @sprintf("特異値は急に小さくなる: ノイズより上に残るモードは %d 個（全 %d 個）", resolved, n),
                        "subtitle" => "左: 時間がたつほど早く落ちる。右: 測定の成分がノイズの高さ（橙の線）で頭打ちになると、その先は取り戻せない"),
        "usermeta" => Dict("singular_values" => s, "resolved" => resolved),
        "resolve" => Dict("scale" => Dict("color" => "independent")),
        "hconcat" => [
            Dict("width" => 310, "height" => 240, "data" => Dict("values" => decay),
                 "mark" => Dict("type" => "line", "strokeWidth" => 2),
                 "encoding" => Dict("x" => Dict("field" => "k", "type" => "quantitative", "title" => "モードの番号 k"),
                                    "y" => Dict("field" => "sigma", "type" => "quantitative", "title" => "特異値",
                                                "scale" => Dict("type" => "log", "domain" => [smallest, 1.5], "nice" => false),
                                                "axis" => Dict("values" => [1e-30, 1e-20, 1e-10, 1], "format" => ".0e")),
                                    "color" => Dict("field" => "time", "type" => "nominal", "title" => "測定の時刻", "sort" => labels,
                                                    "scale" => Dict("range" => ["#1baf7a", "#2a78d6", "#eda100"]),
                                                    "legend" => Dict("orient" => "bottom-left")))),
            Dict("width" => 310, "height" => 240,
                 "layer" => [
                     Dict("data" => Dict("values" => picard), "mark" => Dict("type" => "line", "strokeWidth" => 1.6),
                          "encoding" => Dict("x" => Dict("field" => "k", "type" => "quantitative", "title" => "モードの番号 k"),
                                             "y" => Dict("field" => "v", "type" => "quantitative", "title" => "成分の大きさ（℃）",
                                                         "scale" => Dict("type" => "log", "domain" => [floor_value, 1e3], "nice" => false),
                                                         "axis" => Dict("values" => [1e-6, 1e-4, 1e-2, 1, 100], "format" => "~g")),
                                             "color" => Dict("field" => "series", "type" => "nominal", "title" => nothing,
                                                             "scale" => Dict("range" => ["#111111", "#2a78d6"], "domain" => ["初期温度が残した信号", "測定に含まれる成分"]),
                                                             "legend" => Dict("orient" => "top-right")))),
                     Dict("data" => Dict("values" => [Dict("v" => max(noise, floor_value))]), "mark" => Dict("type" => "rule", "color" => "#eb6834", "strokeDash" => [5, 3], "strokeWidth" => 2),
                          "encoding" => Dict("y" => Dict("field" => "v", "type" => "quantitative"))),
                 ]),
        ]))
end
