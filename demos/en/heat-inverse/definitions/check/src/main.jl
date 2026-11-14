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
    require("Total heat is conserved", change, 1e-10, "relative change")

    # 2. A cosine is an exact mode of the rod: it decays by the analytic factor.
    k = 3
    mode = cos.(k * pi .* x)
    continuous = exp(-alpha * (k * pi)^2 * T)
    discrete = exp(-alpha * 4 * n^2 * sin(k * pi / (2n))^2 * T)
    decay_error = maximum(abs, forward(mode) - discrete * mode)
    require("cos(3πx) decays at the discrete rate", decay_error, 1e-10, @sprintf("difference from the factor %.6f", discrete))
    require("The discrete decay rate is close to the continuous solution", abs(discrete - continuous) / continuous, 0.01, @sprintf("relative difference from the continuous factor %.6f", continuous))

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
    require("Agrees with implicit time integration ($(steps) steps)", maximum(abs, u - forward(truth)), 0.05, "largest difference (°C)")

    # 4. Without noise, and early enough, the inversion recovers the truth.
    early = inputs.params["early_time"]
    W, e = heat_modes(n, alpha, early)
    clean = W * (e .* (W' * truth))
    recovered = W * ((e ./ (e .^ 2 .+ 1e-14)) .* (W' * clean))
    recovery = sqrt(sum(abs2, recovered - truth) / n)
    require(@sprintf("Recovery without noise at an early time T = %g", early), recovery, 1e-6, @sprintf("RMS error (°C), smallest singular value %.3f", e[end]))

    # 5. Heat flows down the gradient: the extremes only move inward.
    clean_later = forward(truth)
    spread = (maximum(clean_later) <= maximum(truth) + 1e-9 && minimum(clean_later) >= minimum(truth) - 1e-9) ? 0.0 : 1.0
    require("Never beyond the highest and lowest temperatures (maximum principle)", spread, 0.0, "any violation (0 = none)")

    report.table(["Checked", "What the value is", "Value", "Tolerance"], rows)

    return nothing
end
