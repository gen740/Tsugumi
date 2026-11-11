using Printf
using Random
using Statistics

# Gillespie's direct method for the birth-death process: nothing -> X at rate
# `birth`, X -> nothing at rate death * n. The count is recorded at `times`.
function simulate!(counts, rng, birth, death, times)
    n = 0
    t = 0.0
    next = 1
    while next <= length(times)
        total = birth + death * n
        t += randexp(rng) / total
        while next <= length(times) && times[next] < t
            counts[next] = n
            next += 1
        end
        n += rand(rng) * total < birth ? 1 : -1
    end
    return counts
end

# Several single runs over the rate equation dn/dt = kV - gamma n, n(0) = 0.
function main(ctx)
    m = input(ctx, "settings")
    k, gamma, volume, runs = m["synthesis"], m["degradation"], m["volume"], Int(m["runs"])
    shown = Int(ctx.params["shown_runs"])
    birth = k * volume
    t_end = 5 / gamma
    times = collect(range(0.0, t_end; length=201))
    rng = Xoshiro(m["seed"])

    ode(t) = birth / gamma * (1 - exp(-gamma * t))
    counts = zeros(Int, length(times), runs)
    for r in 1:runs
        simulate!(view(counts, :, r), rng, birth, gamma, times)
    end
    average = vec(mean(counts; dims=2))
    deviation = maximum(abs.(average .- ode.(times)))
    println(@sprintf("largest gap between the mean of %d runs and the rate equation: %.3f molecules", runs, deviation))

    paths = [Dict("t" => times[i], "n" => counts[i, r], "run" => r) for r in 1:min(shown, runs) for i in eachindex(times)]
    curves = [Dict("t" => t, "n" => round(y; digits=3), "line" => name)
              for (name, ys) in (("Rate equation (deterministic)", ode.(times)), ("Mean of $runs runs", average)) for (t, y) in zip(times, ys)]
    band = [Dict("t" => t, "low" => max(0.0, ode(t) - sqrt(ode(t))), "high" => ode(t) + sqrt(ode(t))) for t in times]

    display_vegalite(ctx, Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("anchor" => "start", "text" => "Each run fluctuates; the mean follows the rate equation",
                        "subtitle" => @sprintf("k = %g, γ = %g, V = %g. The band is mean ± √mean (the Poisson standard deviation)", k, gamma, volume)),
        "usermeta" => Dict("times" => times, "average" => average, "ode" => ode.(times), "runs" => runs),
        "layer" => [
            Dict("data" => Dict("values" => band), "mark" => Dict("type" => "area", "color" => "#8a8880", "opacity" => 0.18),
                 "encoding" => Dict("x" => Dict("field" => "t", "type" => "quantitative", "title" => "Time"),
                                    "y" => Dict("field" => "low", "type" => "quantitative", "title" => "Number of molecules"), "y2" => Dict("field" => "high"))),
            Dict("data" => Dict("values" => paths), "mark" => Dict("type" => "line", "interpolate" => "step-after", "strokeWidth" => 0.9, "opacity" => 0.65, "color" => "#2a78d6"),
                 "encoding" => Dict("x" => Dict("field" => "t", "type" => "quantitative"), "y" => Dict("field" => "n", "type" => "quantitative"),
                                    "detail" => Dict("field" => "run", "type" => "nominal"))),
            Dict("data" => Dict("values" => curves), "mark" => Dict("type" => "line", "strokeWidth" => 2.4),
                 "encoding" => Dict("x" => Dict("field" => "t", "type" => "quantitative"), "y" => Dict("field" => "n", "type" => "quantitative"),
                                    "color" => Dict("field" => "line", "type" => "nominal", "title" => nothing,
                                                    "scale" => Dict("domain" => ["Rate equation (deterministic)", "Mean of $runs runs"], "range" => ["#111111", "#eb6834"]),
                                                    "legend" => Dict("orient" => "bottom-right")),
                                    "strokeDash" => Dict("field" => "line", "type" => "nominal", "legend" => nothing,
                                                         "scale" => Dict("domain" => ["Rate equation (deterministic)", "Mean of $runs runs"], "range" => [[1, 0], [5, 3]])))),
        ]))
end
