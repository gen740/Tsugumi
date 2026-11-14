# Models and integration used by this Card.

# H: prey (hare), L: predator (lynx), in thousands of pelts; t in years.
lotka_volterra(H, L, p) = (p[1] * H - p[2] * H * L,
                           p[3] * H * L - p[4] * L)

# The classic fourth-order Runge-Kutta step.
function rk4_step(rhs::F, H, L, p, dt) where {F}
    k1 = rhs(H, L, p)
    k2 = rhs(H + dt / 2 * k1[1], L + dt / 2 * k1[2], p)
    k3 = rhs(H + dt / 2 * k2[1], L + dt / 2 * k2[2], p)
    k4 = rhs(H + dt * k3[1], L + dt * k3[2], p)
    return (H + dt / 6 * (k1[1] + 2k2[1] + 2k3[1] + k4[1]),
            L + dt / 6 * (k1[2] + 2k2[2] + 2k3[2] + k4[2]))
end
