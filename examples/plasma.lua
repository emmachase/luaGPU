-- plasma.lua
-- Classic plasma effect: sum of overlapping sine waves in UV space,
-- palette-mapped through a smooth colour ramp.

local myShader = shader(function(u_time, u_resolution)

    -- Map a value in [0,1] to a vivid RGB colour using 3 phase-shifted cosines.
    local function palette(t)
        local r = 0.5 + 0.5 * cos(6.2832 * (t + 0.00))
        local g = 0.5 + 0.5 * cos(6.2832 * (t + 0.33))
        local b = 0.5 + 0.5 * cos(6.2832 * (t + 0.67))
        return vec3(r, g, b)
    end

    -- Combine several sine waves to produce a plasma value in [0,1].
    local function plasma(p, t)
        local v = sin(p.x * 4.0 + t)
        v = v + sin(p.y * 4.0 + t * 1.3)
        v = v + sin((p.x + p.y) * 3.0 + t * 0.7)
        v = v + sin(length(p) * 5.0 - t * 1.1)
        return 0.5 + 0.5 * sin(v)
    end

    return function main(uv)
        -- Remap uv from [0,1]^2 to [-1,1]^2, correct for aspect ratio.
        local aspect = u_resolution.x / u_resolution.y
        local p = vec2((uv.x - 0.5) * aspect * 2.0, (uv.y - 0.5) * 2.0)

        local v = plasma(p, u_time)
        local col = palette(v)
        return vec4(col, 1.0)
    end
end)
