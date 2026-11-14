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

function main(inputs, report)
    f = inputs.fits
    years = Int(inputs.params["years"])
    steps = Float64.(inputs.params["dt"])
    lv = f["models"][1]
    p = Float64.(lv["parameters"])
    rows = Vector{Any}[]

    previous = NaN
    for dt in steps
        d = drift(p, years, dt)
        verdict = isnan(previous) ? "基準" : @sprintf("半分の刻みで 1/%.1f（4 次なら 1/16 に近づく）", previous / d)
        push!(rows, [@sprintf("RK4 の保存量 V のずれ、dt = %g 年、%d 年間", dt, years), @sprintf("%.2e", d), "0", verdict])
        isnan(previous) || previous / d > 8 || error(@sprintf("halving dt shrank the drift only %.1f times", previous / d))
        previous = d
    end
    d = drift(p, years, 0.05)
    d < 1e-3 || error("the drift at the demo's dt is $d")

    if f["fit_until"] == 1920
        for (name, value) in zip(lv["parameter_names"], p)
            median, low, high = getfield(STAN, Symbol(name))
            inside = low <= value <= high
            push!(rows, ["LV の $(name) と Stan の事例研究", @sprintf("%.3f", value), @sprintf("%.3f [%.3f, %.3f]", median, low, high),
                         inside ? "10〜90% の範囲内" : "範囲外"])
        end
    else
        push!(rows, ["LV と Stan の事例研究", "fit_until = $(f["fit_until"])", "1900〜1920 年の値", "全期間でないので比べない"])
    end
    report.table(["確かめたこと", "値", "基準", "判定"], rows)

    return nothing
end
