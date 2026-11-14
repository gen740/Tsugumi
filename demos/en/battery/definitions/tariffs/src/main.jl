using Printf
include("dp.jl")

# The same battery under four contracts. With a flat price and equal selling
# price, cycling only loses energy: the optimum is to stay still.
function main(inputs, report)
    m = Tsugumi.input(inputs, "scenario")
    net_load = Float64.(m["load"]) .- Float64.(m["solar"])
    capacity, ratio = m["capacity_kwh"], m["export_ratio"]
    percent = @sprintf("%.0f%%", 100ratio)
    scenarios = (("Flat, exports at the same price", "flat", 1.0), ("Flat, exports $percent", "flat", ratio),
                 ("Time-of-use, exports $percent", "tou", ratio), ("Market-linked, exports $percent", "spot", ratio))

    results = Dict{String,Any}[]
    paths = Dict{String,Any}[]
    for (name, key, sell) in scenarios
        price = Float64.(m[key])
        best = optimal_schedule(net_load, price, sell; capacity, power=m["power_kw"], efficiency=m["round_trip_efficiency"], step=m["soc_step_kwh"])
        saved = no_battery_bill(net_load, price, sell) - best.bill
        used = cycles(best.charge, capacity)
        report.log(string(@sprintf("%s: saved %.2f yen, %.2f cycles", name, saved, used)))
        push!(results, Dict("scenario" => name, "key" => key, "export_ratio" => sell, "saved" => saved, "cycles" => used,
                            "label" => @sprintf("%.0f yen · %.1f cycles", saved, used)))
        append!(paths, [Dict("day" => (t - 1) / 24, "kWh" => best.charge[t], "scenario" => name) for t in eachindex(best.charge)])
    end
    order = [s[1] for s in scenarios]
    colors = Dict("domain" => order, "range" => ["#8a8880", "#eda100", "#1baf7a", "#2a78d6"])

    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => "The shape of the tariff decides the operation",
                        "subtitle" => @sprintf("The same battery (%.1f kWh, round-trip efficiency %.0f%%) run optimally under four contracts", capacity, 100m["round_trip_efficiency"])),
        "usermeta" => Dict("results" => results),
        "vconcat" => [
            Dict("width" => 620, "height" => 100, "data" => Dict("values" => results),
                 "layer" => [
                     Dict("mark" => Dict("type" => "bar", "cornerRadiusEnd" => 3, "height" => 16),
                          "encoding" => Dict("y" => Dict("field" => "scenario", "type" => "nominal", "sort" => order, "title" => nothing),
                                             "x" => Dict("field" => "saved", "type" => "quantitative", "title" => "Saving in a week (yen)", "axis" => Dict("tickCount" => 6)),
                                             "color" => Dict("field" => "scenario", "type" => "nominal", "scale" => colors, "legend" => nothing))),
                     Dict("mark" => Dict("type" => "text", "align" => "left", "dx" => 5, "color" => "#333333"),
                          "encoding" => Dict("y" => Dict("field" => "scenario", "type" => "nominal", "sort" => order, "title" => nothing),
                                             "x" => Dict("field" => "saved", "type" => "quantitative"),
                                             "text" => Dict("field" => "label", "type" => "nominal"))),
                 ]),
            Dict("data" => Dict("values" => paths),
                 "facet" => Dict("field" => "scenario", "type" => "nominal", "sort" => order,
                                 "header" => Dict("title" => nothing, "labelAngle" => 0, "labelAlign" => "left", "labelOrient" => "top", "labelFontSize" => 11)),
                 "columns" => 1, "spacing" => 6,
                 "spec" => Dict("width" => 620, "height" => 50,
                                "mark" => Dict("type" => "line", "strokeWidth" => 1.6),
                                "encoding" => Dict("x" => Dict("field" => "day", "type" => "quantitative", "title" => "Day"),
                                                   "y" => Dict("field" => "kWh", "type" => "quantitative", "title" => "Stored energy (kWh)", "scale" => Dict("domain" => [0, capacity]), "axis" => Dict("tickCount" => 3)),
                                                   "color" => Dict("field" => "scenario", "type" => "nominal", "scale" => colors, "legend" => nothing)))),
        ]))

    return nothing
end
