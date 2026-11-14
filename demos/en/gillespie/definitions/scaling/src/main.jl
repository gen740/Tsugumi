using Printf
using Random
using Statistics

function final_count(rng, birth, death, t_end)
    n = 0
    t = 0.0
    while true
        total = birth + death * n
        t += randexp(rng) / total
        t >= t_end && return n
        n += rand(rng) * total < birth ? 1 : -1
    end
end

# One run in concentration n / V: the same rate equation for every volume.
function concentration_path(rng, birth, death, volume, times)
    n = 0
    t = 0.0
    out = zeros(length(times))
    next = 1
    while next <= length(times)
        total = birth + death * n
        t += randexp(rng) / total
        while next <= length(times) && times[next] < t
            out[next] = n / volume
            next += 1
        end
        n += rand(rng) * total < birth ? 1 : -1
    end
    return out
end

# Fewer molecules, more noise: the coefficient of variation is 1 / sqrt(mean).
function main(inputs, report)
    outputs = Dict{String,Any}()
    m = Tsugumi.input(inputs, "settings")
    k, gamma, runs = m["synthesis"], m["degradation"], Int(m["runs"])
    volumes = sort(Float64.(inputs.params["volumes"]))
    shown = Float64.(inputs.params["shown_volumes"])
    rng = Xoshiro(m["seed"] + 2)
    t_end = 8 / gamma

    means = Float64[]
    cvs = Float64[]
    for volume in volumes
        samples = [final_count(rng, k * volume, gamma, t_end) for _ in 1:runs]
        push!(means, mean(samples))
        push!(cvs, std(samples) / mean(samples))
        report.log(string(@sprintf("V = %g: mean %.2f, CV %.4f, 1/sqrt(mean) %.4f", volume, means[end], cvs[end], 1 / sqrt(k * volume / gamma))))
    end
    theory_mean = k .* volumes ./ gamma
    # The relative standard error of a sample standard deviation is about 1 / sqrt(2 runs)
    # (for very few molecules it is a little larger).
    outputs["scaling"] = Dict("volumes" => volumes, "means" => means, "cvs" => cvs, "theory_cvs" => 1 ./ sqrt.(theory_mean), "runs" => runs)

    points = [Dict("N" => theory_mean[i], "cv" => cvs[i], "low" => cvs[i] * (1 - 2 / sqrt(2runs)), "high" => cvs[i] * (1 + 2 / sqrt(2runs)), "volume" => volumes[i]) for i in eachindex(volumes)]
    fine = exp.(range(log(minimum(theory_mean)), log(maximum(theory_mean)); length=40))
    line = [Dict("N" => N, "cv" => 1 / sqrt(N)) for N in fine]

    times = collect(range(0.0, 4 / gamma; length=161))
    labels = [@sprintf("V = %g", v) for v in shown]
    paths = Dict{String,Any}[]
    for (volume, label) in zip(shown, labels)
        c = concentration_path(rng, k * volume, gamma, volume, times)
        append!(paths, [Dict("t" => times[i], "c" => c[i], "volume" => label) for i in eachindex(times)])
    end
    rate = [Dict("t" => t, "c" => k / gamma * (1 - exp(-gamma * t))) for t in times]

    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => "The fewer the molecules, the larger the fluctuations (CV ∝ 1/√N)",
                        "subtitle" => "Left: in concentrations, the rate equation does not depend on volume. Right: coefficients of variation and the theoretical line; whiskers are about 95% intervals"),
        "usermeta" => Dict("volumes" => volumes, "means" => means, "cvs" => cvs),
        "resolve" => Dict("scale" => Dict("color" => "independent")),
        "hconcat" => [
            Dict("width" => 340, "height" => 240,
                 "layer" => [
                     Dict("data" => Dict("values" => paths), "mark" => Dict("type" => "line", "interpolate" => "step-after", "strokeWidth" => 1.1),
                          "encoding" => Dict("x" => Dict("field" => "t", "type" => "quantitative", "title" => "Time"),
                                             "y" => Dict("field" => "c", "type" => "quantitative", "title" => "Concentration n / V"),
                                             "color" => Dict("field" => "volume", "type" => "nominal", "title" => "Volume", "sort" => labels,
                                                             "scale" => Dict("range" => ["#eda100", "#1baf7a", "#2a78d6"]),
                                                             "legend" => Dict("orient" => "bottom-right")))),
                     Dict("data" => Dict("values" => rate), "mark" => Dict("type" => "line", "color" => "#111111", "strokeWidth" => 2.2),
                          "encoding" => Dict("x" => Dict("field" => "t", "type" => "quantitative"), "y" => Dict("field" => "c", "type" => "quantitative"))),
                 ]),
            Dict("width" => 300, "height" => 240,
                 "layer" => [
                     Dict("data" => Dict("values" => line), "mark" => Dict("type" => "line", "color" => "#eb6834", "strokeWidth" => 2),
                          "encoding" => Dict("x" => Dict("field" => "N", "type" => "quantitative", "title" => "Mean number of molecules N",
                                                         "scale" => Dict("type" => "log", "nice" => false, "domain" => [minimum(theory_mean) / 1.5, maximum(theory_mean) * 1.5]),
                                                         "axis" => Dict("values" => [0.1, 0.3, 1, 3, 10, 30, 100, 300, 1000, 3000, 10000])),
                                             "y" => Dict("field" => "cv", "type" => "quantitative", "title" => "Coefficient of variation CV",
                                                         "scale" => Dict("type" => "log", "nice" => false, "domain" => [minimum(cvs) / 1.5, maximum(cvs) * 1.5]),
                                                         "axis" => Dict("values" => [0.01, 0.03, 0.1, 0.3, 1, 3, 10])))),
                     Dict("data" => Dict("values" => points), "mark" => Dict("type" => "rule", "color" => "#2a78d6"),
                          "encoding" => Dict("x" => Dict("field" => "N", "type" => "quantitative"),
                                             "y" => Dict("field" => "low", "type" => "quantitative"), "y2" => Dict("field" => "high"))),
                     Dict("data" => Dict("values" => points), "mark" => Dict("type" => "point", "filled" => true, "size" => 60, "color" => "#2a78d6"),
                          "encoding" => Dict("x" => Dict("field" => "N", "type" => "quantitative"), "y" => Dict("field" => "cv", "type" => "quantitative"))),
                 ]),
        ]))

    return outputs
end
