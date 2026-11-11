using Printf
include("dp.jl")

function main(ctx)
    m = input(ctx, "scenario")
    tariff = ctx.params["tariff"]
    tariff in ("flat", "tou", "spot") || error("tariff is flat, tou or spot, not $tariff")
    price = Float64.(m[tariff])
    net_load = Float64.(m["load"]) .- Float64.(m["solar"])
    export_ratio = m["export_ratio"]
    capacity, efficiency = m["capacity_kwh"], m["round_trip_efficiency"]

    best = optimal_schedule(net_load, price, export_ratio; capacity, power=m["power_kw"], efficiency, step=m["soc_step_kwh"])
    baseline = no_battery_bill(net_load, price, export_ratio)
    saved = baseline - best.bill
    used = cycles(best.charge, capacity)
    println(@sprintf("%s: without battery %.1f yen, with battery %.1f yen, saved %.1f yen, %.2f cycles", tariff, baseline, best.bill, saved, used))

    day(t) = (t - 1) / 24
    prices = [Dict("day" => day(t), "price" => round(price[t]; digits=2)) for t in eachindex(price)]
    flows = [Dict("day" => day(t), "kWh" => round(best.charge[t + 1] - best.charge[t]; digits=3),
                  "kind" => best.charge[t + 1] >= best.charge[t] ? "Charge" : "Discharge") for t in eachindex(price)]
    levels = [Dict("day" => day(t), "kWh" => best.charge[t]) for t in eachindex(best.charge)]
    xaxis(title) = Dict("field" => "day", "type" => "quantitative", "title" => title, "scale" => Dict("domain" => [0, length(price) / 24]))
    name = Dict("flat" => "flat", "tou" => "time-of-use", "spot" => "market-linked")[tariff]
    display_vegalite(ctx, Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => @sprintf("Optimal operation, %s tariff: %.0f yen saved in a week", name, saved),
                        "subtitle" => @sprintf("Without a battery %.0f yen → with one %.0f yen, %.1f kWh · %.1f kW · round-trip efficiency %.0f%%, %.1f cycles", baseline, best.bill, capacity, m["power_kw"], 100efficiency, used)),
        "usermeta" => Dict("tariff" => tariff, "baseline" => baseline, "bill" => best.bill, "saved" => saved, "cycles" => used,
                           "charge" => best.charge, "grid" => best.grid),
        "vconcat" => [
            Dict("width" => 620, "height" => 90, "data" => Dict("values" => prices),
                 "mark" => Dict("type" => "line", "interpolate" => "step-after", "color" => "#2a78d6", "strokeWidth" => 1.8),
                 "encoding" => Dict("x" => xaxis(""), "y" => Dict("field" => "price", "type" => "quantitative", "title" => "Price (yen/kWh)"))),
            Dict("width" => 620, "height" => 90, "data" => Dict("values" => flows),
                 "mark" => Dict("type" => "bar", "width" => 2.5),
                 "encoding" => Dict("x" => xaxis(""), "y" => Dict("field" => "kWh", "type" => "quantitative", "title" => "Charge / discharge (kWh)"),
                                    "color" => Dict("field" => "kind", "type" => "nominal", "title" => "Direction",
                                                    "scale" => Dict("domain" => ["Charge", "Discharge"], "range" => ["#1baf7a", "#eb6834"])))),
            Dict("width" => 620, "height" => 90, "data" => Dict("values" => levels),
                 "mark" => Dict("type" => "area", "color" => "#2a78d6", "opacity" => 0.35, "line" => Dict("color" => "#2a78d6")),
                 "encoding" => Dict("x" => xaxis("Day"), "y" => Dict("field" => "kWh", "type" => "quantitative", "title" => "Stored energy (kWh)",
                                                                    "scale" => Dict("domain" => [0, capacity])))),
        ]))
end
