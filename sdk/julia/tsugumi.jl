# Tsugumi Julia Card SDK. A Card provides `main(ctx)` in src/main.jl:
#
#   function main(ctx)
#       n = input(ctx, "number")["value"]
#       output(ctx, "result", Dict("value" => n * ctx.params["factor"]))
#       display_markdown(ctx, "**$n**")
#   end
#
# The worker uses the same FlatBuffers ABI as C++/Python. Its in-memory adapter
# presents messages as Dicts; no files or subprocess protocol carry values.
# Module state persists with reuseWorker. Displays publish during the call.
module Tsugumi

export input, path, output, display_text, display_markdown, display_table, display_image, display_module, display_vegalite, cancelled, remaining_ms

# ---- JSON (the standard library has none) ------------------------------------

function parse_json(s::AbstractString)
    text = String(s)
    value, at = parse_value(text, skip_space(text, firstindex(text)))
    skip_space(text, at) > ncodeunits(text) || error("JSON: trailing text at byte $at")
    return value
end

function skip_space(s, i)
    while i <= ncodeunits(s) && codeunit(s, i) in (0x20, 0x09, 0x0a, 0x0d)
        i += 1
    end
    return i
end

function parse_value(s, i)
    i <= ncodeunits(s) || error("JSON: unexpected end")
    c = s[i]
    c == '{' && return parse_object(s, i)
    c == '[' && return parse_array(s, i)
    c == '"' && return parse_string(s, i)
    startswith(SubString(s, i), "true") && return true, i + 4
    startswith(SubString(s, i), "false") && return false, i + 5
    startswith(SubString(s, i), "null") && return nothing, i + 4
    stop = i
    while stop <= ncodeunits(s) && codeunit(s, stop) in codeunits("+-.0123456789eE")
        stop += 1
    end
    stop > i || error("JSON: unexpected character at byte $i")
    number = SubString(s, i, stop - 1)
    integer = tryparse(Int64, number)
    return (integer === nothing ? something(tryparse(UInt64, number), parse(Float64, number)) : integer), stop
end

function parse_object(s, i)
    object = Dict{String,Any}()
    i = skip_space(s, i + 1)
    s[i] == '}' && return object, i + 1
    while true
        key, i = parse_string(s, skip_space(s, i))
        i = skip_space(s, i)
        s[i] == ':' || error("JSON: expected ':' at byte $i")
        value, i = parse_value(s, skip_space(s, i + 1))
        object[key] = value
        i = skip_space(s, i)
        s[i] == '}' && return object, i + 1
        s[i] == ',' || error("JSON: expected ',' at byte $i")
        i += 1
    end
end

function parse_array(s, i)
    array = Any[]
    i = skip_space(s, i + 1)
    s[i] == ']' && return array, i + 1
    while true
        value, i = parse_value(s, skip_space(s, i))
        push!(array, value)
        i = skip_space(s, i)
        s[i] == ']' && return array, i + 1
        s[i] == ',' || error("JSON: expected ',' at byte $i")
        i += 1
    end
end

function parse_string(s, i)
    s[i] == '"' || error("JSON: expected a string at byte $i")
    out = IOBuffer()
    i = nextind(s, i)
    while true
        c = s[i]
        if c == '"'
            return String(take!(out)), nextind(s, i)
        elseif c == '\\'
            i = nextind(s, i)
            e = s[i]
            if e == 'u'
                code = parse(UInt16, SubString(s, i + 1, i + 4); base=16)
                i += 4
                if 0xd800 <= code < 0xdc00  # a surrogate pair
                    low = parse(UInt16, SubString(s, i + 3, i + 6); base=16)
                    write(out, Char(0x10000 + ((UInt32(code) - 0xd800) << 10) + (low - 0xdc00)))
                    i += 6
                else
                    write(out, Char(code))
                end
            else
                write(out, get(Dict('n' => '\n', 't' => '\t', 'r' => '\r', 'b' => '\b', 'f' => '\f'), e, e))
            end
        else
            write(out, c)
        end
        i = nextind(s, i)
    end
end

write_json(io, v::AbstractString) = (print(io, '"'); escape_json(io, v); print(io, '"'))
write_json(io, v::Symbol) = write_json(io, String(v))
write_json(io, v::Bool) = print(io, v)
write_json(io, v::Integer) = print(io, v)
write_json(io, v::AbstractFloat) = isfinite(v) ? print(io, Float64(v)) : print(io, "null")
write_json(io, ::Nothing) = print(io, "null")
function write_json(io, v::AbstractDict)
    print(io, '{')
    # FlatBuffers requires a union discriminator before its value. Dict order is unspecified.
    ordered = sort!(collect(keys(v)); by=key -> (!endswith(string(key), "_type"), string(key)))
    for (n, key) in enumerate(ordered)
        value = v[key]
        n > 1 && print(io, ',')
        write_json(io, string(key))
        print(io, ':')
        write_json(io, value)
    end
    print(io, '}')
end
function write_json(io, v::Union{AbstractVector,Tuple})
    print(io, '[')
    for (n, value) in enumerate(v)
        n > 1 && print(io, ',')
        write_json(io, value)
    end
    print(io, ']')
end

function escape_json(io, s)
    for c in s
        if c == '"' || c == '\\'
            print(io, '\\', c)
        elseif c < ' '
            print(io, "\\u", string(UInt16(c); base=16, pad=4))
        else
            print(io, c)
        end
    end
end

json_text(v) = (io = IOBuffer(); write_json(io, v); String(take!(io)))

# ---- The Card's side of the worker ---------------------------------------------

const HOST = ENV["TSUGUMI_NATIVE_LIBRARY"]

struct Context
    params::Dict{String,Any}
    output_directory::String
    temporary_directory::String
    working_directory::String
    run_id::String
    card::String
    invocation::UInt64
end

function info(field)
    value = ccall((:tsugumi_native_info, HOST), Cstring, (Cint,), field)
    value == C_NULL && error("no active Card invocation")
    return unsafe_string(value)
end

function Context()
    value = ccall((:tsugumi_native_params, HOST), Cstring, ())
    value == C_NULL && error("no active Card invocation")
    Context(parse_json(unsafe_string(value)), info(4), info(3), info(2), info(0), info(1),
            ccall((:tsugumi_native_invocation, HOST), UInt64, ()))
end

function input(ctx::Context, port::AbstractString)
    value = ccall((:tsugumi_native_input, HOST), Cstring, (Cstring,), port)
    value == C_NULL && return nothing
    return parse_json(unsafe_string(value))
end

function path(ctx::Context, port::AbstractString)
    ccall((:tsugumi_native_input_kind, HOST), Cint, (Cstring,), port) in (1, 2) || error("no File or Directory input port named $port")
    value = input(ctx, port)
    value !== nothing && get(value, "value", nothing) isa AbstractString || error("no File or Directory input port named $port")
    return value["value"]
end

function output(ctx::Context, port::AbstractString, message)
    ccall((:tsugumi_native_output, HOST), Cint, (Cstring, Cstring), port, json_text(message)) == 0 || error("output was rejected by the worker: $port")
    return nothing
end

cancelled(ctx::Context) = ccall((:tsugumi_native_cancelled, HOST), Cint, ()) != 0
remaining_ms(ctx::Context) = ccall((:tsugumi_native_remaining_ms, HOST), Int64, ())

function show_member(member)
    ccall((:tsugumi_native_display, HOST), Cint, (Cstring,), json_text(member)) == 0 || error("display was rejected by the worker")
    return nothing
end

display_text(ctx::Context, text::AbstractString) = show_member(Dict("text" => text))
display_markdown(ctx::Context, text::AbstractString) = show_member(Dict("markdown" => text))
display_image(ctx::Context, file::AbstractString; alt::AbstractString="") = show_member(Dict("image" => Dict("path" => file, "alt" => alt)))
display_module(ctx::Context, name::AbstractString, data) = show_member(Dict("module" => Dict("name" => name, "data" => data)))
display_vegalite(ctx::Context, spec) = display_module(ctx, "vega-lite", spec)

# columns: names; rows: vectors of cells, shown with `string`.
function display_table(ctx::Context, columns, rows)
    return show_member(Dict("table" => Dict("columns" => collect(string.(columns)), "rows" => [collect(string.(row)) for row in rows])))
end

function invoke(hook::Bool)
    try
        ctx = Context()
        if hook
            if isdefined(Main, :on_run_start)
                Base.invokelatest(Main.on_run_start, ctx)
            end
        else
            Base.invokelatest(Main.main, ctx)
        end
        return true
    catch error
        message = sprint(showerror, error, catch_backtrace())
        ccall((:tsugumi_native_fail, HOST), Cvoid, (Cstring,), message)
        return false
    end
end

end # module Tsugumi
