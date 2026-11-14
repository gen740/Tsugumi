# Models and integration used by this Card.

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
    (name="ロトカ・ヴォルテラ", rhs=lotka_volterra,
     names=["a", "b", "c", "d"], low=[0.2, 0.005, 0.002, 0.2], high=[2.0, 0.1, 0.05, 2.0]),
    (name="被食者ロジスティック", rhs=logistic_prey,
     names=["a", "b", "c", "d", "K"], low=[0.2, 0.005, 0.002, 0.2, 50.0], high=[2.0, 0.1, 0.05, 2.0, 1000.0]),
    (name="ホリング II 型", rhs=holling_ii,
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
