using Printf

# The real record, bundled with the Card: the number of pelts the Hudson's Bay
# Company took each year. Pelts are a proxy for animals, not a census.
function main(inputs, report)
    outputs = Dict{String,Any}()
    years, lynx, hare = Int[], Float64[], Float64[]
    for line in eachline(joinpath(@__DIR__, "hudson-bay-lynx-hare.csv"))
        (startswith(line, "#") || startswith(line, "Year")) && continue
        y, l, h = split(line, ",")
        push!(years, parse(Int, y))
        push!(lynx, parse(Float64, l))
        push!(hare, parse(Float64, h))
    end
    report.log(string(@sprintf("%d years from %d to %d; largest hare %.1f, largest lynx %.1f (thousands of pelts)", length(years), years[1], years[end], maximum(hare), maximum(lynx))))
    outputs["pelts"] = Dict("years" => years, "hare" => hare, "lynx" => lynx)

    rows = [Dict("year" => years[i], "pelts" => series[i], "species" => name)
            for (name, series) in (("Hare (prey)", hare), ("Lynx (predator)", lynx)) for i in eachindex(years)]
    report.vegalite(Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("anchor" => "start", "text" => "The Hudson's Bay Company's fur records ($(years[1])–$(years[end]), $(length(years)) years)",
                        "subtitle" => "Real data. Source: stan-dev/example-models (from Howard 2009's lecture notes). Not population counts but pelts traded"),
        "usermeta" => Dict("years" => years, "hare" => hare, "lynx" => lynx),
        "data" => Dict("values" => rows),
        "mark" => Dict("type" => "line", "strokeWidth" => 2, "point" => true),
        "encoding" => Dict("x" => Dict("field" => "year", "type" => "quantitative", "title" => "Year", "scale" => Dict("zero" => false), "axis" => Dict("format" => "d")),
                           "y" => Dict("field" => "pelts", "type" => "quantitative", "title" => "Pelts (thousands)"),
                           "color" => Dict("field" => "species", "type" => "nominal", "title" => nothing,
                                           "scale" => Dict("domain" => ["Hare (prey)", "Lynx (predator)"], "range" => ["#2a78d6", "#eb6834"]),
                                           "legend" => Dict("orient" => "top-right")))))

    return outputs
end
