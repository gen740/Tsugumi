using Printf
using Random
using Statistics

# A + B <-> C from `total` molecules of A and of B, and no C: the time-averaged
# count of C after a burn-in. The association propensity is on / V * a * b.
function average_bound(rng, total, on_rate, off_rate, t_burn, t_end)
    c = 0
    t = 0.0
    weighted = 0.0
    while true
        forward = on_rate * (total - c)^2
        backward = off_rate * c
        sum_rates = forward + backward
        t_next = t + randexp(rng) / sum_rates
        weighted += c * max(0.0, min(t_next, t_end) - max(t, t_burn))
        t_next >= t_end && return weighted / (t_end - t_burn)
        t = t_next
        c += rand(rng) * sum_rates < forward ? 1 : -1
    end
end

# The exact stationary mean, from detailed balance:
# P(c + 1) off (c + 1) = P(c) (on / V) (total - c)^2.
function exact_mean(total, on_rate, off_rate)
    logp = 0.0
    logs = [0.0]
    for c in 0:total-1
        logp += 2 * log(total - c) + log(on_rate) - log(off_rate) - log(c + 1)
        push!(logs, logp)
    end
    shift = maximum(logs)
    weights = exp.(logs .- shift)
    return sum((0:total) .* weights) / sum(weights)
end

# The mass-action equilibrium: on / V (total - c)^2 = off c.
function rate_equation_bound(total, on_rate, off_rate)
    r = off_rate / on_rate
    return ((2total + r) - sqrt((2total + r)^2 - 4total^2)) / 2
end

# A nonlinear reaction: the rate equation is the large-volume limit only. In a
# small volume the true mean differs from it, and the gap closes as V grows.
function main(inputs, report)
    outputs = Dict{String,Any}()
    m = inputs.settings
    concentration = inputs.params["concentration"]
    on, off = inputs.params["on_rate"], inputs.params["off_rate"]
    volumes = sort(Float64.(inputs.params["volumes"]))
    replicas = Int(inputs.params["replicas"])
    rng = Xoshiro(m["seed"] + 3)

    rows = Dict{String,Any}[]
    ssa_fraction = Float64[]
    ssa_se = Float64[]
    exact_fraction = Float64[]
    ode_fraction = Float64[]
    totals = Int[]
    for volume in volumes
        total = round(Int, concentration * volume)
        on_rate = on / volume
        t_end = 400 / (off + on * concentration)
        samples = [average_bound(rng, total, on_rate, off, t_end / 4, t_end) / total for _ in 1:replicas]
        exact = exact_mean(total, on_rate, off) / total
        ode = rate_equation_bound(total, on_rate, off) / total
        push!(totals, total)
        push!(ssa_fraction, mean(samples))
        push!(ssa_se, std(samples) / sqrt(replicas))
        push!(exact_fraction, exact)
        push!(ode_fraction, ode)
        report.log(string(@sprintf("V = %g, %d molecules each: SSA %.4f +- %.4f, exact %.4f, rate equation %.4f", volume, total, ssa_fraction[end], ssa_se[end], exact, ode)))
        push!(rows, Dict("total" => total, "low" => ssa_fraction[end] - 2ssa_se[end], "high" => ssa_fraction[end] + 2ssa_se[end], "ssa" => ssa_fraction[end]))
    end
    outputs["binding"] = Dict("totals" => totals, "ssa" => ssa_fraction, "se" => ssa_se, "exact" => exact_fraction, "ode" => ode_fraction)

    lines = [Dict("total" => totals[i], "fraction" => ys[i], "line" => name)
             for (name, ys) in (("Rate equation", ode_fraction), ("Exact (detailed balance)", exact_fraction)) for i in eachindex(totals)]
    x = Dict("field" => "total", "type" => "quantitative", "title" => "Molecules of A and of B (proportional to volume)",
             "scale" => Dict("type" => "log", "nice" => false, "domain" => [totals[1] / 1.2, totals[end] * 1.2]))
    y_scale = Dict("zero" => false)
    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("anchor" => "start", "text" => "A non-linear reaction A + B ⇌ C: in small volumes it departs from the rate equation",
                        "subtitle" => @sprintf("Same concentration %g, kon = %g, koff = %g. Points are SSA (mean of %d runs, whiskers ±2 standard errors)", concentration, on, off, replicas)),
        "usermeta" => Dict("totals" => totals, "ssa" => ssa_fraction, "exact" => exact_fraction, "ode" => ode_fraction),
        "layer" => [
            Dict("data" => Dict("values" => lines), "mark" => Dict("type" => "line", "strokeWidth" => 2),
                 "encoding" => Dict("x" => x, "y" => Dict("field" => "fraction", "type" => "quantitative", "title" => "Fraction bound ⟨C⟩ / total", "scale" => y_scale),
                                    "color" => Dict("field" => "line", "type" => "nominal", "title" => nothing,
                                                    "scale" => Dict("domain" => ["Rate equation", "Exact (detailed balance)"], "range" => ["#111111", "#eb6834"]),
                                                    "legend" => Dict("orient" => "top-right")))),
            Dict("data" => Dict("values" => rows), "mark" => Dict("type" => "rule", "color" => "#2a78d6"),
                 "encoding" => Dict("x" => x, "y" => Dict("field" => "low", "type" => "quantitative"), "y2" => Dict("field" => "high"))),
            Dict("data" => Dict("values" => rows), "mark" => Dict("type" => "point", "filled" => true, "size" => 55, "color" => "#2a78d6"),
                 "encoding" => Dict("x" => x, "y" => Dict("field" => "ssa", "type" => "quantitative"))),
        ]))

    return outputs
end
