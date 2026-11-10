using Printf

# The real record, bundled with the Card: the number of pelts the Hudson's Bay
# Company took each year. Pelts are a proxy for animals, not a census.
function main(ctx)
    years, lynx, hare = Int[], Float64[], Float64[]
    for line in eachline(joinpath(@__DIR__, "hudson-bay-lynx-hare.csv"))
        (startswith(line, "#") || startswith(line, "Year")) && continue
        y, l, h = split(line, ",")
        push!(years, parse(Int, y))
        push!(lynx, parse(Float64, l))
        push!(hare, parse(Float64, h))
    end
    println(@sprintf("%d years from %d to %d; largest hare %.1f, largest lynx %.1f (thousands of pelts)", length(years), years[1], years[end], maximum(hare), maximum(lynx)))
    output(ctx, "pelts", Dict("years" => years, "hare" => hare, "lynx" => lynx))

    rows = [Dict("year" => years[i], "pelts" => series[i], "species" => name)
            for (name, series) in (("ノウサギ（被食者）", hare), ("オオヤマネコ（捕食者）", lynx)) for i in eachindex(years)]
    display_vegalite(ctx, Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "background" => "white",
        "title" => Dict("anchor" => "start", "text" => "ハドソン湾会社の毛皮の記録（$(years[1])〜$(years[end]) 年、$(length(years)) 年分）",
                        "subtitle" => "実データ。出典: stan-dev/example-models（Howard 2009 の講義資料から）。個体数ではなく取引された毛皮の数"),
        "usermeta" => Dict("years" => years, "hare" => hare, "lynx" => lynx),
        "data" => Dict("values" => rows),
        "mark" => Dict("type" => "line", "strokeWidth" => 2, "point" => true),
        "encoding" => Dict("x" => Dict("field" => "year", "type" => "quantitative", "title" => "年", "scale" => Dict("zero" => false), "axis" => Dict("format" => "d")),
                           "y" => Dict("field" => "pelts", "type" => "quantitative", "title" => "毛皮の数（千枚）"),
                           "color" => Dict("field" => "species", "type" => "nominal", "title" => nothing,
                                           "scale" => Dict("domain" => ["ノウサギ（被食者）", "オオヤマネコ（捕食者）"], "range" => ["#2a78d6", "#eb6834"]),
                                           "legend" => Dict("orient" => "top-right")))))
end
