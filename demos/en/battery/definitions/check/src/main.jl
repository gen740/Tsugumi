using Printf
include("dp.jl")

# Tries every path of charge levels over a short horizon: no shortcut, no table.
function brute_force(net_load, price, export_ratio; capacity, power, efficiency, step)
    levels = round(Int, capacity / step)
    oneway = sqrt(efficiency)
    start = levels ÷ 2
    up = floor(Int, power * oneway / step + 1e-9)
    down = floor(Int, power / oneway / step + 1e-9)
    hours = length(net_load)
    paths = 0
    function walk(t, s, bill)
        if t > hours
            paths += 1
            return s == start ? bill : Inf
        end
        best = Inf
        for to in max(0, s - down):min(levels, s + up)
            grid = net_load[t] + ac_energy((to - s) * step, oneway)
            best = min(best, walk(t + 1, to, bill + hour_bill(grid, price[t], export_ratio * price[t])))
        end
        return best
    end
    return walk(1, start, 0.0), paths
end

function main(inputs, report)
    m = inputs.scenario
    hours = Int(inputs.params["brute_hours"])
    first_hour = Int(inputs.params["brute_from_hour"]) + 1
    window = first_hour:first_hour+hours-1
    net_load = Float64.(m["load"]) .- Float64.(m["solar"])
    ratio, efficiency = m["export_ratio"], m["round_trip_efficiency"]
    rows = Vector{Any}[]

    # 1. The DP against every path, on a small battery over a short horizon.
    for (name, key) in (("Flat", "flat"), ("Time-of-use", "tou"), ("Market-linked", "spot"))
        price = Float64.(m[key])[window]
        small = (capacity=2.0, power=1.0, efficiency=efficiency, step=0.5)
        dp = optimal_schedule(net_load[window], price, ratio; small...).bill
        brute, paths = brute_force(net_load[window], price, ratio; small...)
        abs(dp - brute) < 1e-8 || error("DP $dp differs from brute force $brute under $name")
        push!(rows, ["DP = exhaustive search ($(name), $(hours) hours, $(paths) paths)", @sprintf("%.4f yen", dp), @sprintf("%.4f yen", brute), "agree"])
    end

    # 2. A flat price with an equal selling price: cycling only loses energy.
    flat = Float64.(m["flat"])
    still = optimal_schedule(net_load, flat, 1.0; capacity=m["capacity_kwh"], power=m["power_kw"], efficiency, step=m["soc_step_kwh"])
    saved = no_battery_bill(net_load, flat, 1.0) - still.bill
    moved = cycles(still.charge, m["capacity_kwh"])
    efficiency < 1 && (abs(saved) < 1e-9 && moved == 0 || error("a flat tariff should leave the battery still"))
    push!(rows, ["Flat, exports at the same price: does not run", @sprintf("saving %.2f yen, %.2f cycles", saved, moved), "0 yen, 0 cycles", "agree"])

    # 3. A finer grid of charge levels can only do better, and soon stops mattering.
    spot = Float64.(m["spot"])
    step = m["soc_step_kwh"]
    previous = -Inf
    for k in 0:3
        fine = optimal_schedule(net_load, spot, ratio; capacity=m["capacity_kwh"], power=m["power_kw"], efficiency, step=step / 2^k)
        s = no_battery_bill(net_load, spot, ratio) - fine.bill
        s >= previous - 1e-6 || error("a finer grid earned less: $s after $previous")
        push!(rows, [@sprintf("Step of stored energy %.4g kWh (market-linked)", step / 2^k), @sprintf("%.2f yen", s), k == 0 ? "—" : @sprintf("%+.2f yen", s - previous), k == 0 ? "reference" : "increases"])
        previous = s
    end
    report.table(["Checked", "Value", "Expected", "Verdict"], rows)

    return nothing
end
