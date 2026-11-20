# Tsugumi Julia Cards return Dicts by output port, or an iterator of Dicts.
# main(inputs[, report[, run]]); presentation functions take report first.
module Tsugumi

export Inputs, Report, input, path, text, markdown, table, image, vegalite, Run, cancelled, remaining_ms

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
    for (token, value) in (("NaN", NaN), ("Infinity", Inf), ("-Infinity", -Inf))
        startswith(SubString(s, i), token) && return value, i + ncodeunits(token)
    end
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
write_json(io, v::AbstractFloat) = print(io, isnan(v) ? "NaN" : isinf(v) ? (signbit(v) ? "-Infinity" : "Infinity") : Float64(v))
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

const HOST = Main.TSUGUMI_HOST

mutable struct CallScope
    active::Bool
    owner::Task
end
const active_scope = Ref{Union{Nothing,CallScope}}(nothing)

struct Run
    scope::CallScope
    output_directory::String
    temporary_directory::String
    working_directory::String
    run_id::String
    card::String
    invocation::UInt64
end
function info(field)
    value = ccall(HOST.info, Cstring, (Cint,), field)
    value == C_NULL && error("no active Card invocation")
    return unsafe_string(value)
end

function Run()
    scope = active_scope[]
    scope === nothing && error("no active Card invocation")
    Run(scope, info(4), info(3), info(2), info(0), info(1),
            ccall(HOST.invocation, UInt64, ()))
end


struct Report end
function Base.getproperty(report::Report, name::Symbol)
    method = name == :module ? module_view : getfield(Tsugumi, name)
    return (args...; kwargs...) -> method(report, args...; kwargs...)
end
struct Inputs
    params::Dict{String,Any}
    values::Dict{String,Any}
end
function Inputs()
    value = ccall(HOST.params, Cstring, ())
    value == C_NULL && error("no active Card invocation")
    inputs = ccall(HOST.inputs, Cstring, ())
    inputs == C_NULL && error("cannot decode inputs")
    Inputs(parse_json(unsafe_string(value)), parse_json(unsafe_string(inputs)))
end
Base.getproperty(inputs::Inputs, name::Symbol) = name == :params ? deepcopy(getfield(inputs, :params)) : input(inputs, string(name))

function input(inputs::Inputs, port::AbstractString)
    deepcopy(get(getfield(inputs, :values), port, nothing))
end

function path(inputs::Inputs, port::AbstractString)
    ccall(HOST.input_kind, Cint, (Cstring,), port) in (1, 2) || error("no File or Directory input port named $port")
    value = input(inputs, port)
    value !== nothing && get(value, "value", nothing) isa AbstractString || error("no File or Directory input port named $port")
    return value["value"]
end

function check(run::Run)
    run.scope.active && run.scope === active_scope[] && run.scope.owner === current_task() || error("Card call has expired or belongs to another task")
end
cancelled(run::Run) = (check(run); ccall(HOST.cancelled, Cint, ()) != 0)
remaining_ms(run::Run) = (check(run); ccall(HOST.remaining_ms, Int64, ()))
log(report::Report, message::AbstractString) = ccall(HOST.log, Cvoid, (Cstring,), message)

function show_member(member)
    ccall(HOST.display, Cint, (Cstring,), json_text(member)) == 0 || error("display was rejected by the worker")
    return nothing
end

text(report::Report, text::AbstractString) = show_member(Dict("text" => text))
markdown(report::Report, text::AbstractString) = show_member(Dict("markdown" => text))
image(report::Report, file::AbstractString; alt::AbstractString="") = show_member(Dict("image" => Dict("path" => file, "alt" => alt)))
module_view(report::Report, name::AbstractString, data) = show_member(Dict("module" => Dict("module" => name, "config" => data)))
vegalite(report::Report, spec) = module_view(report, "vega-lite", spec)

# columns: names; rows: vectors of cells, shown with `string`.
function table(report::Report, columns, rows)
    return show_member(Dict("table" => Dict("columns" => collect(string.(columns)), "rows" => [collect(string.(row)) for row in rows])))
end

function invoke(hook::Bool)
    scope = CallScope(true, current_task())
    active_scope[] = scope
    try
        arguments = (Inputs(), Report(), Run())
        function_ = hook ? (isdefined(Main, :on_run_start) ? Main.on_run_start : nothing) : Main.main
        result = nothing
        if function_ !== nothing
            for count in 1:3
                args = arguments[1:count]
                if applicable(function_, args...)
                    result = Base.invokelatest(function_, args...)
                    break
                end
                count == 3 && error("Card functions take inputs, optionally report and run")
            end
        end
        hook && return true
        result === nothing && return true
        result isa AbstractDict || error("return a Dict by output port")
        for (port, value) in result
            value === nothing && continue
            ccall(HOST.output, Cint, (Cstring, Cstring), string(port), json_text(value)) == 0 || return false
        end
        return true
    catch error
        message = sprint(showerror, error, catch_backtrace())
        ccall(HOST.fail, Cvoid, (Cstring,), message)
        return false
    finally
        scope.active = false
        active_scope[] = nothing
    end
end

end # module Tsugumi
