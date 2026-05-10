-- voronoi.lua
-- Animated Voronoi / Worley noise with coloured cells and sharp borders.
-- Each cell centre drifts over time; the closest two distances give
-- the border thickness and a smooth interior gradient.

local myShader = shader(function(u_time, u_resolution)

    -- A fast deterministic hash from a vec2 integer cell coordinate to vec2.
    local function hash2(p)
        local x = sin(dot(p, vec2(127.1, 311.7))) * 43758.5
        local y = sin(dot(p, vec2(269.5, 183.3))) * 43758.5
        return vec2(fract(x), fract(y))
    end

    -- Cell colour based on integer cell id.
    local function cell_colour(id)
        local h = fract(sin(dot(id, vec2(13.7, 47.3))) * 21341.7)
        local r = 0.4 + 0.6 * fract(h * 3.17)
        local g = 0.4 + 0.6 * fract(h * 5.71)
        local b = 0.4 + 0.6 * fract(h * 9.43)
        return vec3(r, g, b)
    end

    -- Worley noise: returns {min_dist, second_min_dist, winning_cell_id_x, id_y}
    -- We encode it as a vec4 to stay within the type system.
    local function voronoi(p)
        local i_p = floor(p)  -- integer cell
        local f_p = fract(p)  -- position within cell

        local d1  = 9999.0   -- closest distance
        local d2  = 9999.0   -- second closest
        local id_x = 0.0
        local id_y = 0.0

        local dy = -2
        while dy <= 2 do
            local dx = -2
            while dx <= 2 do
                local neighbour = vec2(float(dx), float(dy))
                local cell_id   = i_p + neighbour
                -- Animated cell centre: hash gives base offset, time drifts it.
                local base = hash2(cell_id)
                local centre = neighbour + base +
                               0.5 * sin(u_time * 0.8 + 6.2832 * base)

                local d = length(f_p - centre)

                if d < d1 then
                    d2   = d1
                    d1   = d
                    id_x = cell_id.x
                    id_y = cell_id.y
                end
                if d < d2 and d > d1 then
                    d2 = d
                end

                dx = dx + 1
            end
            dy = dy + 1
        end

        return vec4(d1, d2, id_x, id_y)
    end

    return function main(uv)
        local aspect = u_resolution.x / u_resolution.y
        local scale  = 6.0
        local p = vec2(uv.x * aspect, uv.y) * scale

        local v = voronoi(p)
        local d1  = v.x
        local d2  = v.y
        local idx = v.z
        local idy = v.w

        -- Base cell colour.
        local col = cell_colour(vec2(idx, idy))

        -- Darken toward cell borders: thin when d2-d1 is small.
        local border = smoothstep(0.0, 0.08, d2 - d1)
        col = col * border

        -- Vignette within each cell: slightly darker toward centre of mass.
        local inner = 1.0 - smoothstep(0.0, 0.5, d1)
        col = col * (0.7 + 0.3 * inner)

        -- Subtle animated pulse on the closest-cell distance.
        local pulse = 0.5 + 0.5 * sin(u_time * 2.0 - d1 * 8.0)
        col = col + vec3(0.05, 0.03, 0.08) * pulse * (1.0 - border)

        return vec4(clamp(col, 0.0, 1.0), 1.0)
    end
end)
