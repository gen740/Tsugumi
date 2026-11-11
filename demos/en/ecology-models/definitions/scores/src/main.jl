using Printf

# All the scores side by side: fit, penalized fit and forecast.
function main(ctx)
    f = input(ctx, "fits")
    h = input(ctx, "forecast")
    models = f["models"]
    best_aicc = minimum(m["aicc"] for m in models)
    best_bic = minimum(m["bic"] for m in models)
    rows = Vector{Any}[]
    for (i, m) in enumerate(models)
        @assert h["names"][i] == m["name"]
        parameters = join((@sprintf("%s=%.3g", n, v) for (n, v) in zip(m["parameter_names"], m["parameters"])), " ")
        push!(rows, [m["name"], string(m["k"]), @sprintf("%.3f", m["rmse_log"]),
                     @sprintf("%.1f", m["aic"]), @sprintf("%.1f", m["aicc"]), @sprintf("%+.1f", m["aicc"] - best_aicc),
                     @sprintf("%.1f", m["bic"]), @sprintf("%+.1f", m["bic"] - best_bic),
                     @sprintf("%.3f", h["rmse_heldout"][i]), "$(m["agreeing"]) / $(m["restarts"])", parameters])
    end
    display_table(ctx, ["Model", "Parameters", "Log RMSE", "AIC", "AICc", "ΔAICc", "BIC", "ΔBIC",
                        "Prediction log RMSE ($(h["fit_until"] + 1)–$(h["last_year"]))", "Starts reaching the same solution", "Best values"], rows)
end
