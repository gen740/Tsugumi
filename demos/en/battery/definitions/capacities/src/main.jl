using Printf
include("dp.jl")

# How much each added kWh of battery is worth, and how the schedule changes with size.
function main(ctx)
    m = input(ctx, "scenario")
    sizes = sort(Float64.(ctx.params["capacities_kwh"]))
    shown = Float64.(ctx.params["schedule_sizes_kwh"])
    net_load = Float64.(m["load"]) .- Float64.(m["solar"])
    solve(price, capacity) = optimal_schedule(net_load, price, m["export_ratio"]; capacity, power=m["power_kw"],
                                              efficiency=m["round_trip_efficiency"], step=m["soc_step_kwh"])

    savings = Dict{String,Vector{Float64}}()
    rows = Dict{String,Any}[]
    marginal = Dict{String,Any}[]
    for (name, key) in (("Market-linked", "spot"), ("Time-of-use", "tou"))
        price = Float64.(m[key])
        baseline = no_battery_bill(net_load, price, m["export_ratio"])
        saved = [baseline - solve(price, c).bill for c in sizes]
        savings[key] = saved
        for i in eachindex(sizes)
            push!(rows, Dict("kWh" => sizes[i], "saved" => round(saved[i]; digits=1), "tariff" => name))
            i > 1 && push!(marginal, Dict("kWh" => (sizes[i] + sizes[i - 1]) / 2, "per" => round((saved[i] - saved[i - 1]) / (sizes[i] - sizes[i - 1]); digits=1), "tariff" => name))
        end
        println(name, ": ", join((@sprintf("%g kWh %.0f yen", sizes[i], saved[i]) for i in eachindex(sizes)), ", "))
    end

    # The first three days of the schedule under the market tariff, at a few sizes.
    labels = [@sprintf("%g kWh", c) for c in shown]
    paths = Dict{String,Any}[]
    for (c, label) in zip(shown, labels)
        charge = solve(Float64.(m["spot"]), c).charge
        for t in 1:min(73, length(charge))
            push!(paths, Dict("day" => (t - 1) / 24, "share" => c > 0 ? round(charge[t] / c; digits=3) : 0.0, "size" => label))
        end
    end

    tariff_colors = Dict("domain" => ["Market-linked", "Time-of-use"], "range" => ["#2a78d6", "#1baf7a"])
    lines = Dict("type" => "line", "point" => true, "strokeWidth" => 1.8)
    display_vegalite(ctx, Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => "More capacity, but the saving levels off",
                        "subtitle" => @sprintf("A made-up week, changing only the capacity at a power limit of %.1f kW", m["power_kw"])),
        "usermeta" => Dict("capacities" => sizes, "savings" => savings),
        "resolve" => Dict("scale" => Dict("color" => "independent")),
        "vconcat" => [
            Dict("hconcat" => [
                Dict("width" => 300, "height" => 170, "data" => Dict("values" => rows), "mark" => lines,
                     "encoding" => Dict("x" => Dict("field" => "kWh", "type" => "quantitative", "title" => "Capacity (kWh)"),
                                        "y" => Dict("field" => "saved", "type" => "quantitative", "title" => "Saving in a week (yen)"),
                                        "color" => Dict("field" => "tariff", "type" => "nominal", "legend" => nothing, "scale" => tariff_colors))),
                Dict("width" => 300, "height" => 170, "data" => Dict("values" => marginal), "mark" => lines,
                     "encoding" => Dict("x" => Dict("field" => "kWh", "type" => "quantitative", "title" => "Capacity (kWh, middle of the interval)"),
                                        "y" => Dict("field" => "per", "type" => "quantitative", "title" => "Saving per additional kWh (yen)"),
                                        "color" => Dict("field" => "tariff", "type" => "nominal", "title" => "Tariff", "scale" => tariff_colors))),
            ], "resolve" => Dict("scale" => Dict("color" => "independent"))),
            Dict("width" => 680, "height" => 120, "data" => Dict("values" => paths),
                 "title" => Dict("text" => "First 3 days, market-linked: state of charge by capacity", "fontSize" => 12),
                 "mark" => Dict("type" => "line", "strokeWidth" => 1.8),
                 "encoding" => Dict("x" => Dict("field" => "day", "type" => "quantitative", "title" => "Day"),
                                    "y" => Dict("field" => "share", "type" => "quantitative", "title" => "State of charge", "axis" => Dict("format" => ".0%")),
                                    "color" => Dict("field" => "size", "type" => "nominal", "title" => "Capacity", "sort" => labels,
                                                    "scale" => Dict("range" => ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"])))),
        ]))
end
