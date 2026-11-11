using Printf
include("ecology.jl")

# Lotka-Volterra has a conserved quantity, so the integrator can be tested without
# any data: V = c H - d log H + b L - a log L stays constant along an exact orbit.
conserved(H, L, p) = p[3] * H - p[4] * log(H) + p[2] * L - p[1] * log(L)

function drift(p, years, dt)
    H, L = p[end - 1], p[end]
    start = conserved(H, L, p)
    worst = 0.0
    for _ in 1:round(Int, years / dt)
        H, L = rk4_step(lotka_volterra, H, L, p, dt)
        worst = max(worst, abs(conserved(H, L, p) - start))
    end
    return worst
end

# Bob Carpenter's Stan case study "Predator-Prey Population Dynamics" fits the same
# model to the same 21 years by Bayesian inference: posterior median and 10-90% interval.
const STAN = (a=(0.542, 0.465, 0.630), b=(0.027, 0.022, 0.033), c=(0.024, 0.020, 0.029), d=(0.797, 0.692, 0.926),
              H0=(33.908, 30.415, 37.630), L0=(5.912, 5.273, 6.614))

function main(ctx)
    f = input(ctx, "fits")
    years = Int(ctx.params["years"])
    steps = Float64.(ctx.params["dt"])
    lv = f["models"][1]
    p = Float64.(lv["parameters"])
    rows = Vector{Any}[]

    previous = NaN
    for dt in steps
        d = drift(p, years, dt)
        verdict = isnan(previous) ? "reference" : @sprintf("1/%.1f at half the step (approaches 1/16 for fourth order)", previous / d)
        push!(rows, [@sprintf("Drift of RK4's conserved quantity V, dt = %g years, %d years", dt, years), @sprintf("%.2e", d), "0", verdict])
        isnan(previous) || previous / d > 8 || error(@sprintf("halving dt shrank the drift only %.1f times", previous / d))
        previous = d
    end
    d = drift(p, years, 0.05)
    d < 1e-3 || error("the drift at the demo's dt is $d")

    if f["fit_until"] == 1920
        for (name, value) in zip(lv["parameter_names"], p)
            median, low, high = getfield(STAN, Symbol(name))
            inside = low <= value <= high
            push!(rows, ["LV's $(name) and the Stan case study", @sprintf("%.3f", value), @sprintf("%.3f [%.3f, %.3f]", median, low, high),
                         inside ? "within 10–90%" : "outside"])
        end
    else
        push!(rows, ["LV and the Stan case study", "fit_until = $(f["fit_until"])", "values for 1900–1920", "not the whole period, so not compared"])
    end
    display_table(ctx, ["Checked", "Value", "Expected", "Verdict"], rows)
end
