using Random

# A synthetic week, hour by hour: household load, rooftop solar and three price
# lists (yen per kWh). Nothing here is measured; the seed fixes every number.
function synthetic_week(seed, days)
    rng = Xoshiro(seed)
    hours = 24 * days
    bump(h, centre, width) = exp(-((h - centre) / width)^2)

    # Each day has its own cloud cover, which also moves the market's midday price.
    clear = [0.3 + 0.7 * rand(rng) for _ in 1:days]
    load = zeros(hours)
    solar = zeros(hours)
    spot = zeros(hours)
    noise = 0.0
    for t in 1:hours
        h = (t - 1) % 24
        sun = clear[(t - 1) ÷ 24 + 1]
        load[t] = (0.35 + 0.9 * bump(h, 7.5, 1.5) + 1.5 * bump(h, 19.5, 2.5)) * (0.85 + 0.3 * rand(rng))
        solar[t] = 4.0 * sun * max(0.0, sin(pi * (h - 6) / 12))^1.5
        noise = 0.7 * noise + 1.5 * randn(rng)
        spot[t] = max(1.0, 16.0 + 7.0 * bump(h, 8, 2) + 18.0 * bump(h, 19, 2.5) - 9.0 * sun * bump(h, 12.5, 3.5) + noise)
    end
    flat = fill(30.0, hours)
    tou = [(t - 1) % 24 < 7 || (t - 1) % 24 >= 23 ? 18.0 : (t - 1) % 24 < 17 ? 30.0 : 42.0 for t in 1:hours]
    return (; load, solar, flat, tou, spot)
end

function main(inputs, report)
    outputs = Dict{String,Any}()
    s = inputs.settings
    week = synthetic_week(s["seed"], Int(s["days"]))
    outputs["scenario"] = merge(s, Dict(
        "load" => week.load, "solar" => week.solar, "flat" => week.flat, "tou" => week.tou, "spot" => week.spot))

    day(t) = (t - 1) / 24
    prices = [Dict("day" => day(t), "tariff" => name, "price" => round(series[t]; digits=2))
              for (name, series) in (("Flat", week.flat), ("Time-of-use", week.tou), ("Market-linked", week.spot))
              for t in eachindex(series)]
    power = [Dict("day" => day(t), "kind" => name, "kW" => round(series[t]; digits=3))
             for (name, series) in (("House consumption", week.load), ("Solar", week.solar)) for t in eachindex(series)]
    xaxis = Dict("field" => "day", "type" => "quantitative", "title" => "Day", "axis" => Dict("tickMinStep" => 1))
    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => "A made-up week (data from random numbers, seed = $(s["seed"]))",
        "usermeta" => Dict("load" => week.load, "solar" => week.solar, "flat" => week.flat, "tou" => week.tou, "spot" => week.spot),
        "resolve" => Dict("scale" => Dict("color" => "independent")),
        "vconcat" => [
            Dict("width" => 620, "height" => 130, "data" => Dict("values" => prices), "mark" => Dict("type" => "line", "strokeWidth" => 1.8, "interpolate" => "step-after"),
                 "encoding" => Dict("x" => xaxis, "y" => Dict("field" => "price", "type" => "quantitative", "title" => "Price (yen/kWh)"),
                                    "color" => Dict("field" => "tariff", "type" => "nominal", "title" => "Tariff",
                                                    "scale" => Dict("domain" => ["Flat", "Time-of-use", "Market-linked"], "range" => ["#8a8880", "#1baf7a", "#2a78d6"])))),
            Dict("width" => 620, "height" => 110, "data" => Dict("values" => power), "mark" => Dict("type" => "line", "strokeWidth" => 1.8),
                 "encoding" => Dict("x" => xaxis, "y" => Dict("field" => "kW", "type" => "quantitative", "title" => "Power (kW)"),
                                    "color" => Dict("field" => "kind", "type" => "nominal", "title" => "Power",
                                                    "scale" => Dict("domain" => ["House consumption", "Solar"], "range" => ["#eb6834", "#eda100"])))),
        ]))

    return outputs
end
