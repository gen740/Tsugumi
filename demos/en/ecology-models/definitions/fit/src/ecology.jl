# The models, integrator and optimizer used by this Card.
using Random

# H: prey (hare), L: predator (lynx), in thousands of pelts; t in years.
lotka_volterra(H, L, p) = (p[1] * H - p[2] * H * L,
                           p[3] * H * L - p[4] * L)

logistic_prey(H, L, p) = (p[1] * H * (1 - H / p[5]) - p[2] * H * L,
                          p[3] * H * L - p[4] * L)

function holling_ii(H, L, p)
    eaten = p[2] * H / (1 + p[2] * p[6] * H) # per predator: saturates once handling takes the time
    return (p[1] * H * (1 - H / p[5]) - eaten * L,
            p[3] * eaten * L - p[4] * L)
end

# Each model: its right-hand side, and for each parameter a name and a range to start from.
const MODELS = [
    (name="Lotka–Volterra", rhs=lotka_volterra,
     names=["a", "b", "c", "d"], low=[0.2, 0.005, 0.002, 0.2], high=[2.0, 0.1, 0.05, 2.0]),
    (name="Logistic prey", rhs=logistic_prey,
     names=["a", "b", "c", "d", "K"], low=[0.2, 0.005, 0.002, 0.2, 50.0], high=[2.0, 0.1, 0.05, 2.0, 1000.0]),
    (name="Holling type II", rhs=holling_ii,
     names=["a", "b", "e", "d", "K", "h"], low=[0.2, 0.005, 0.05, 0.2, 50.0, 0.01], high=[2.0, 0.2, 1.0, 2.0, 1000.0, 2.0]),
]

# The classic fourth-order Runge-Kutta step.
function rk4_step(rhs::F, H, L, p, dt) where {F}
    k1 = rhs(H, L, p)
    k2 = rhs(H + dt / 2 * k1[1], L + dt / 2 * k1[2], p)
    k3 = rhs(H + dt / 2 * k2[1], L + dt / 2 * k2[2], p)
    k4 = rhs(H + dt * k3[1], L + dt * k3[2], p)
    return (H + dt / 6 * (k1[1] + 2k2[1] + 2k3[1] + k4[1]),
            L + dt / 6 * (k1[2] + 2k2[2] + 2k3[2] + k4[2]))
end

# The state at t = 0, 1/per_year, 2/per_year, ... up to `years`.
function simulate(rhs::F, p, H0, L0, years; per_year=1, dt=0.05) where {F}
    steps = round(Int, 1 / (per_year * dt))
    count = years * per_year + 1
    Hs = zeros(count)
    Ls = zeros(count)
    H, L = H0, L0
    Hs[1], Ls[1] = H, L
    for i in 2:count
        for _ in 1:steps
            H, L = rk4_step(rhs, H, L, p, dt)
        end
        Hs[i], Ls[i] = H, L
    end
    return Hs, Ls
end

# The sum of squared errors of log counts: pelts scatter by a factor, not by a fixed amount.
function log_squared_error(rhs::F, x, hare, lynx, dt) where {F}
    p = exp.(x)
    H, L = p[end - 1], p[end]
    total = 0.0
    for i in eachindex(hare)
        if i > 1
            for _ in 1:round(Int, 1 / dt)
                H, L = rk4_step(rhs, H, L, p, dt)
            end
        end
        (H > 1e-6 && L > 1e-6 && H < 1e7 && L < 1e7) || return 1e9
        total += (log(hare[i]) - log(H))^2 + (log(lynx[i]) - log(L))^2
    end
    return total
end

# Nelder-Mead on the logarithms of the parameters, so that they stay positive.
function nelder_mead(f, start; size=0.3, max_evaluations=6000, tolerance=1e-9)
    n = length(start)
    points = [copy(start) for _ in 1:n+1]
    for i in 1:n
        points[i + 1][i] += size
    end
    values = f.(points)
    evaluations = n + 1
    while evaluations < max_evaluations
        order = sortperm(values)
        points, values = points[order], values[order]
        values[end] - values[1] < tolerance && break
        centre = sum(points[1:n]) / n
        reflected = centre + (centre - points[end])
        fr = f(reflected)
        evaluations += 1
        if fr < values[1]
            expanded = centre + 2 * (centre - points[end])
            fe = f(expanded)
            evaluations += 1
            points[end], values[end] = fe < fr ? (expanded, fe) : (reflected, fr)
        elseif fr < values[n]
            points[end], values[end] = reflected, fr
        else
            contracted = centre + 0.5 * ((fr < values[end] ? reflected : points[end]) - centre)
            fc = f(contracted)
            evaluations += 1
            if fc < min(fr, values[end])
                points[end], values[end] = contracted, fc
            else
                for i in 2:n+1
                    points[i] = points[1] + 0.5 * (points[i] - points[1])
                    values[i] = f(points[i])
                end
                evaluations += n
            end
        end
    end
    best = argmin(values)
    return points[best], values[best]
end

# Starts Nelder-Mead from `restarts` random points, and restarts it from its best
# result (a simplex can collapse early). `agreeing`: how many starts ended within
# 1% of the best, which says how rugged the error surface is.
function fit_model(model, hare, lynx, restarts, rng, dt)
    f(x) = log_squared_error(model.rhs, x, hare, lynx, dt)
    lows = vcat(model.low, [hare[1] / 2, lynx[1] / 2])
    highs = vcat(model.high, [hare[1] * 2, lynx[1] * 2])
    ends = Float64[]
    best_x, best_error = Float64[], Inf
    for _ in 1:restarts
        start = [log(l) + rand(rng) * (log(h) - log(l)) for (l, h) in zip(lows, highs)]
        x, e = nelder_mead(f, start)
        x, e = nelder_mead(f, x; size=0.1)
        push!(ends, e)
        if e < best_error
            best_x, best_error = x, e
        end
    end
    return (parameters=exp.(best_x), error=best_error, agreeing=count(<=(best_error * 1.01), ends))
end
