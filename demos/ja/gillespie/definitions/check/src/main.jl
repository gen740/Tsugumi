using Printf

# Each simulated value against its closed form. More than `limit` standard errors
# away fails the Card: a seed can be unlucky, a wrong simulation always is.
function main(inputs, report)
    limit = inputs.params["limit_sigma"]
    s = Tsugumi.input(inputs, "stationary")
    c = Tsugumi.input(inputs, "scaling")
    b = Tsugumi.input(inputs, "binding")
    rows = Vector{Any}[]
    function compare(name, value, theory, se)
        z = (value - theory) / se
        abs(z) <= limit || error(@sprintf("%s: %.4f is %.1f standard errors from %.4f", name, value, z, theory))
        push!(rows, [name, @sprintf("%.4f", value), @sprintf("%.4f", theory), @sprintf("%+.2f", z), "範囲内"])
    end

    compare("定常の平均", s["mean"], s["theory"], s["se_mean"])
    compare("定常の分散", s["variance"], s["theory"], s["se_variance"])
    compare("ファノ因子", s["fano"], 1.0, s["se_fano"])
    for i in eachindex(c["volumes"])
        v = c["volumes"][i]
        theory = c["theory_cvs"][i]
        # A standard deviation's relative error, widened for a Poisson's tail at small N.
        n = 1 / theory^2
        se = theory * sqrt((2 + 1 / n) / (4 * c["runs"]))
        compare(@sprintf("CV（V = %g、N = %.3g）", v, n), c["cvs"][i], theory, se)
    end
    for i in eachindex(b["totals"])
        compare(@sprintf("A + B ⇌ C の結合割合（各 %d 個、厳密解と）", b["totals"][i]), b["ssa"][i], b["exact"][i], b["se"][i])
    end
    report.table(["量", "シミュレーション", "閉じた式", "ずれ（標準誤差）", "判定"], rows)

    return nothing
end
