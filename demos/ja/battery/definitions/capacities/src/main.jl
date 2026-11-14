using Printf
include("dp.jl")

# How much each added kWh of battery is worth, and how the schedule changes with size.
function main(inputs, report)
    m = Tsugumi.input(inputs, "scenario")
    sizes = sort(Float64.(inputs.params["capacities_kwh"]))
    shown = Float64.(inputs.params["schedule_sizes_kwh"])
    net_load = Float64.(m["load"]) .- Float64.(m["solar"])
    solve(price, capacity) = optimal_schedule(net_load, price, m["export_ratio"]; capacity, power=m["power_kw"],
                                              efficiency=m["round_trip_efficiency"], step=m["soc_step_kwh"])

    savings = Dict{String,Vector{Float64}}()
    rows = Dict{String,Any}[]
    marginal = Dict{String,Any}[]
    for (name, key) in (("市場連動", "spot"), ("時間帯別", "tou"))
        price = Float64.(m[key])
        baseline = no_battery_bill(net_load, price, m["export_ratio"])
        saved = [baseline - solve(price, c).bill for c in sizes]
        savings[key] = saved
        for i in eachindex(sizes)
            push!(rows, Dict("kWh" => sizes[i], "saved" => round(saved[i]; digits=1), "tariff" => name))
            i > 1 && push!(marginal, Dict("kWh" => (sizes[i] + sizes[i - 1]) / 2, "per" => round((saved[i] - saved[i - 1]) / (sizes[i] - sizes[i - 1]); digits=1), "tariff" => name))
        end
        report.log(string(name, ": ", join((@sprintf("%g kWh %.0f yen", sizes[i], saved[i]) for i in eachindex(sizes)), ", ")))
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

    tariff_colors = Dict("domain" => ["市場連動", "時間帯別"], "range" => ["#2a78d6", "#1baf7a"])
    lines = Dict("type" => "line", "point" => true, "strokeWidth" => 1.8)
    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("text" => "容量を増やしても節約は頭打ちになる",
                        "subtitle" => @sprintf("電力の上限 %.1f kW のまま容量だけを変えた、架空の 1 週間の結果", m["power_kw"])),
        "usermeta" => Dict("capacities" => sizes, "savings" => savings),
        "resolve" => Dict("scale" => Dict("color" => "independent")),
        "vconcat" => [
            Dict("hconcat" => [
                Dict("width" => 300, "height" => 170, "data" => Dict("values" => rows), "mark" => lines,
                     "encoding" => Dict("x" => Dict("field" => "kWh", "type" => "quantitative", "title" => "容量（kWh）"),
                                        "y" => Dict("field" => "saved", "type" => "quantitative", "title" => "1 週間の節約（円）"),
                                        "color" => Dict("field" => "tariff", "type" => "nominal", "legend" => nothing, "scale" => tariff_colors))),
                Dict("width" => 300, "height" => 170, "data" => Dict("values" => marginal), "mark" => lines,
                     "encoding" => Dict("x" => Dict("field" => "kWh", "type" => "quantitative", "title" => "容量（kWh、区間の中央）"),
                                        "y" => Dict("field" => "per", "type" => "quantitative", "title" => "追加 1 kWh あたりの節約（円）"),
                                        "color" => Dict("field" => "tariff", "type" => "nominal", "title" => "料金", "scale" => tariff_colors))),
            ], "resolve" => Dict("scale" => Dict("color" => "independent"))),
            Dict("width" => 680, "height" => 120, "data" => Dict("values" => paths),
                 "title" => Dict("text" => "市場連動の最初の 3 日: 容量ごとの蓄電率", "fontSize" => 12),
                 "mark" => Dict("type" => "line", "strokeWidth" => 1.8),
                 "encoding" => Dict("x" => Dict("field" => "day", "type" => "quantitative", "title" => "日"),
                                    "y" => Dict("field" => "share", "type" => "quantitative", "title" => "蓄電率", "axis" => Dict("format" => ".0%")),
                                    "color" => Dict("field" => "size", "type" => "nominal", "title" => "容量", "sort" => labels,
                                                    "scale" => Dict("range" => ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"])))),
        ]))

    return nothing
end
