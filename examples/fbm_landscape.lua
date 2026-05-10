-- fbm_landscape.lua
-- Fractional Brownian Motion (fBm) used to generate a 2-D terrain heightmap
-- rendered as a top-down colour map with contour lines and a moving sun.
-- Also demonstrates multi-octave loops and the ternary pattern.

local myShader = shader(function(u_time, u_resolution)

    -- 2-D value noise: smooth hash in [0,1].
    local function hash(p)
        return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453)
    end

    local function noise(p)
        local i = floor(p)
        local f = fract(p)
        -- Quintic interpolation.
        local u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0)

        local a = hash(i + vec2(0.0, 0.0))
        local b = hash(i + vec2(1.0, 0.0))
        local c = hash(i + vec2(0.0, 1.0))
        local d = hash(i + vec2(1.0, 1.0))

        return mix(mix(a, b, u.x), mix(c, d, u.x), u.y)
    end

    -- fBm: sum of octaves, each halved in amplitude and doubled in frequency.
    local function fbm(p)
        local value = 0.0
        local amplitude = 0.5
        local frequency = 1.0
        local i = 0
        while i < 6 do
            value     = value + amplitude * noise(p * frequency)
            frequency = frequency * 2.0
            amplitude = amplitude * 0.5
            i = i + 1
        end
        return value
    end

    -- Terrain colour based on height and gradient.
    local function terrain_colour(h, slope)
        -- Deep water / shallow water / beach / lowland / highland / snow.
        local deep_water   = vec3(0.05, 0.15, 0.35)
        local shallow_water= vec3(0.10, 0.30, 0.55)
        local beach        = vec3(0.78, 0.72, 0.52)
        local grass        = vec3(0.25, 0.52, 0.18)
        local rock         = vec3(0.40, 0.35, 0.28)
        local snow         = vec3(0.90, 0.92, 0.96)

        local col = deep_water
        col = (h > 0.30) and shallow_water or col
        col = (h > 0.38) and beach         or col
        col = (h > 0.42) and grass         or col
        col = (h > 0.68) and rock          or col
        col = (h > 0.80) and snow          or col

        -- Steeper slopes are rockier.
        col = mix(col, rock, clamp(slope * 4.0 - 0.5, 0.0, 1.0))
        return col
    end

    return function(uv)
        -- Drift the view slowly over time.
        local offset = vec2(u_time * 0.04, u_time * 0.025)
        local p = uv * 3.5 + offset

        local h = fbm(p)

        -- Approximate slope magnitude via finite differences.
        local eps = 0.004
        local dh_dx = fbm(p + vec2(eps, 0.0)) - fbm(p - vec2(eps, 0.0))
        local dh_dy = fbm(p + vec2(0.0, eps)) - fbm(p - vec2(0.0, eps))
        local slope = length(vec2(dh_dx, dh_dy)) / (2.0 * eps) * 0.01

        local col = terrain_colour(h, slope)

        -- Contour lines every 0.05 height units.
        local contour = abs(fract(h * 20.0) - 0.5)
        local line    = smoothstep(0.03, 0.05, contour)
        col = col * (0.75 + 0.25 * line)

        -- Diffuse sun shading from a fixed direction.
        local sun_dir = normalize(vec2(0.6, 0.4))
        local diffuse = clamp(dh_dx * sun_dir.x + dh_dy * sun_dir.y, 0.0, 1.0)
        col = col * (0.6 + 0.4 * diffuse)

        -- Subtle vignette.
        local vign = 1.0 - 0.5 * length(uv - vec2(0.5, 0.5))
        col = col * vign

        return vec4(clamp(col, 0.0, 1.0), 1.0)
    end
end)

return myShader
