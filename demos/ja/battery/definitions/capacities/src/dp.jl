# The dynamic program used by this Card.
#
# State: the charge of the battery, on a grid of `step` kWh. Each hour it moves
# to any level the power limit reaches, and the bill of that hour follows. The
# battery starts half full and must end where it started, so no stored energy
# is spent for free.

# Half of the round-trip loss is on the way in, half on the way out: `oneway`
# is sqrt(round-trip efficiency). A positive `stored` draws stored / oneway from
# the house's connection; a negative one gives it back -stored * oneway.
ac_energy(stored, oneway) = stored > 0 ? stored / oneway : stored * oneway

# The bill of one hour: what is bought costs `price`, what is sold earns `sell`.
hour_bill(grid, price, sell) = grid > 0 ? price * grid : sell * grid

function optimal_schedule(net_load, price, export_ratio; capacity, power, efficiency, step)
    hours = length(net_load)
    levels = round(Int, capacity / step)
    oneway = sqrt(efficiency)
    start = levels ÷ 2
    up = floor(Int, power * oneway / step + 1e-9)   # the most it can gain in an hour, in levels
    down = floor(Int, power / oneway / step + 1e-9) # the most it can lose

    # Backward: cost_to_go[s + 1] is the least bill from this hour on when the charge is s levels.
    cost_to_go = fill(Inf, levels + 1)
    cost_to_go[start + 1] = 0.0
    choice = zeros(Int, levels + 1, hours)
    for t in hours:-1:1
        next = copy(cost_to_go)
        for s in 0:levels
            best = hour_bill(net_load[t], price[t], export_ratio * price[t]) + next[s + 1] # staying wins ties
            best_to = s
            for to in max(0, s - down):min(levels, s + up)
                grid = net_load[t] + ac_energy((to - s) * step, oneway)
                cost = hour_bill(grid, price[t], export_ratio * price[t]) + next[to + 1]
                if cost < best - 1e-9
                    best, best_to = cost, to
                end
            end
            cost_to_go[s + 1] = best
            choice[s + 1, t] = best_to
        end
    end

    # Forward: follow the choices from the start.
    charge = zeros(hours + 1)
    grid = zeros(hours)
    s = start
    charge[1] = s * step
    for t in 1:hours
        to = choice[s + 1, t]
        grid[t] = net_load[t] + ac_energy((to - s) * step, oneway)
        s = to
        charge[t + 1] = s * step
    end
    bill = sum(hour_bill(grid[t], price[t], export_ratio * price[t]) for t in 1:hours; init=0.0)
    return (; bill, charge, grid)
end

no_battery_bill(net_load, price, export_ratio) =
    sum(hour_bill(net_load[t], price[t], export_ratio * price[t]) for t in eachindex(net_load))
