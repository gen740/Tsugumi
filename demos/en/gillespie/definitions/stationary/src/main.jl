using Printf
using Random
using Statistics

# One run from zero molecules to time t_end: the count at t_end.
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

# The stationary distribution: each run gives one independent sample long after
# the start, and theory says Poisson with mean kV / gamma (so the Fano factor is 1).
function main(ctx)
    m = input(ctx, "settings")
    k, gamma, volume, runs = m["synthesis"], m["degradation"], m["volume"], Int(m["runs"])
    birth = k * volume
    expected = birth / gamma
    t_end = 12 / gamma
    rng = Xoshiro(m["seed"] + 1)
    samples = [final_count(rng, birth, gamma, t_end) for _ in 1:runs]

    average, variance = mean(samples), var(samples)
    fano = variance / average
    # Standard errors of the mean and the variance of a Poisson sample.
    se_mean = sqrt(expected / runs)
    se_var = sqrt((expected + 2 * expected^2) / runs)
    se_fano = sqrt(2 / runs)
    println(@sprintf("mean %.3f (theory %.3f), variance %.3f (theory %.3f), Fano %.4f", average, expected, variance, expected, fano))
    output(ctx, "stationary", Dict("mean" => average, "variance" => variance, "fano" => fano, "theory" => expected,
                                   "se_mean" => se_mean, "se_variance" => se_var, "se_fano" => se_fano, "runs" => runs))

    top = max(maximum(samples), ceil(Int, expected + 4 * sqrt(expected)))
    bottom = min(minimum(samples), max(0, floor(Int, expected - 4 * sqrt(expected))))
    # Poisson probabilities by the recursion p(n + 1) = p(n) lambda / (n + 1), in logs.
    logp = -expected
    poisson = Dict{String,Any}[]
    for n in 0:top
        n > 0 && (logp += log(expected / n))
        n >= bottom && push!(poisson, Dict("n" => n, "p" => exp(logp), "line" => "Poisson distribution (theory)"))
    end
    hist = [Dict("from" => n - 0.45, "to" => n + 0.45, "p" => count(==(n), samples) / runs) for n in bottom:top]
    xscale = Dict("domain" => [bottom - 0.6, top + 0.6], "nice" => false)

    display_vegalite(ctx, Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("anchor" => "start", "text" => "The stationary distribution is Poisson",
                        "subtitle" => @sprintf("End points of %d runs: mean %.2f (theory %.2f), variance %.2f (theory %.2f), Fano factor %.3f (theory 1)", runs, average, expected, variance, expected, fano)),
        "usermeta" => Dict("mean" => average, "variance" => variance, "fano" => fano, "theory" => expected, "runs" => runs),
        "layer" => [
            Dict("data" => Dict("values" => hist), "mark" => Dict("type" => "rect", "color" => "#2a78d6", "opacity" => 0.55),
                 "encoding" => Dict("x" => Dict("field" => "from", "type" => "quantitative", "title" => "Number of molecules", "scale" => xscale), "x2" => Dict("field" => "to"),
                                    "y" => Dict("field" => "p", "type" => "quantitative", "title" => "Fraction"), "y2" => Dict("datum" => 0))),
            Dict("data" => Dict("values" => poisson), "mark" => Dict("type" => "line", "strokeWidth" => 2.2, "point" => Dict("size" => 18, "filled" => true)),
                 "encoding" => Dict("x" => Dict("field" => "n", "type" => "quantitative", "scale" => xscale), "y" => Dict("field" => "p", "type" => "quantitative"),
                                    "color" => Dict("field" => "line", "type" => "nominal", "title" => nothing, "scale" => Dict("range" => ["#eb6834"]),
                                                    "legend" => Dict("orient" => "top-right")))),
        ]))
end
