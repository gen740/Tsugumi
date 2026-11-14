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

# Each check has a known answer, independent of the noise in this run.
function main(inputs, report)
    m = inputs.measurement
    x, truth = Float64.(m["x"]), Float64.(m["truth"])
    n = length(x)
    alpha, T = m["alpha"], m["final_time"]
    V, s = heat_modes(n, alpha, T)
    forward(u) = V * (s .* (V' * u))
    rows = Vector{Any}[]
    function require(name, value, limit, text)
        value <= limit || error(@sprintf("%s: %.3e is above %.1e", name, value, limit))
        push!(rows, [name, text, @sprintf("%.2e", value), @sprintf("%.0e", limit)])
    end

    # 1. Insulated ends keep the total heat.
    change = abs(sum(forward(truth)) - sum(truth)) / sum(truth)
    require("総熱量が保存する", change, 1e-10, "相対変化")

    # 2. A cosine is an exact mode of the rod: it decays by the analytic factor.
    k = 3
    mode = cos.(k * pi .* x)
    continuous = exp(-alpha * (k * pi)^2 * T)
    discrete = exp(-alpha * 4 * n^2 * sin(k * pi / (2n))^2 * T)
    decay_error = maximum(abs, forward(mode) - discrete * mode)
    require("cos(3πx) が離散の減衰率で減る", decay_error, 1e-10, @sprintf("倍率 %.6f との差", discrete))
    require("離散の減衰率が連続の解に近い", abs(discrete - continuous) / continuous, 0.01, @sprintf("連続の倍率 %.6f との相対差", continuous))

    # 3. The eigendecomposition agrees with small implicit time steps.
    steps = 2000
    dt = T / steps
    h = 1 / n
    d = fill(2.0, n)
    d[1] = d[n] = 1.0
    B = I + alpha * dt * SymTridiagonal(d ./ h^2, fill(-1.0 / h^2, n - 1))
    u = copy(truth)
    for _ in 1:steps
        u = B \ u
    end
    require("陰解法の時間積分（$(steps) ステップ）と一致", maximum(abs, u - forward(truth)), 0.05, "最大の差（℃）")

    # 4. Without noise, and early enough, the inversion recovers the truth.
    early = inputs.params["early_time"]
    W, e = heat_modes(n, alpha, early)
    clean = W * (e .* (W' * truth))
    recovered = W * ((e ./ (e .^ 2 .+ 1e-14)) .* (W' * clean))
    recovery = sqrt(sum(abs2, recovered - truth) / n)
    require(@sprintf("ノイズなし・早い時刻 T = %g の復元", early), recovery, 1e-6, @sprintf("RMS 誤差（℃）、最小の特異値 %.3f", e[end]))

    # 5. Heat flows down the gradient: the extremes only move inward.
    clean_later = forward(truth)
    spread = (maximum(clean_later) <= maximum(truth) + 1e-9 && minimum(clean_later) >= minimum(truth) - 1e-9) ? 0.0 : 1.0
    require("最高・最低温度を超えない（最大値原理）", spread, 0.0, "外れの有無（0 = なし）")

    report.table(["確かめたこと", "何の値か", "値", "許容"], rows)

    return nothing
end
