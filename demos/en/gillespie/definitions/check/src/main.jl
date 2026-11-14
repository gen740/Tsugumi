using Printf

# Each simulated value against its closed form. More than `limit` standard errors
# away fails the Card: a seed can be unlucky, a wrong simulation always is.
function main(inputs, report)
    limit = inputs.params["limit_sigma"]
    s = inputs.stationary
    c = inputs.scaling
    b = inputs.binding
    rows = Vector{Any}[]
    function compare(name, value, theory, se)
        z = (value - theory) / se
        abs(z) <= limit || error(@sprintf("%s: %.4f is %.1f standard errors from %.4f", name, value, z, theory))
        push!(rows, [name, @sprintf("%.4f", value), @sprintf("%.4f", theory), @sprintf("%+.2f", z), "within"])
    end

    compare("Stationary mean", s["mean"], s["theory"], s["se_mean"])
    compare("Stationary variance", s["variance"], s["theory"], s["se_variance"])
    compare("Fano factor", s["fano"], 1.0, s["se_fano"])
    for i in eachindex(c["volumes"])
        v = c["volumes"][i]
        theory = c["theory_cvs"][i]
        # A standard deviation's relative error, widened for a Poisson's tail at small N.
        n = 1 / theory^2
        se = theory * sqrt((2 + 1 / n) / (4 * c["runs"]))
        compare(@sprintf("CV (V = %g, N = %.3g)", v, n), c["cvs"][i], theory, se)
    end
    for i in eachindex(b["totals"])
        compare(@sprintf("Fraction bound of A + B ⇌ C (%d each, against the exact solution)", b["totals"][i]), b["ssa"][i], b["exact"][i], b["se"][i])
    end
    report.table(["Quantity", "Simulation", "Closed form", "Off by (standard errors)", "Verdict"], rows)

    return nothing
end
