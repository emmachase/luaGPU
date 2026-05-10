-- mandelbrot.lua
-- Mandelbrot set with smooth colouring and animated zoom.
-- Iterates z = z^2 + c and colours by escape-time.

local myShader = shader(function(u_time, u_resolution)

    -- Complex multiply: (a + bi)(c + di) = (ac-bd, ad+bc)
    local function cmul(a, b)
        return vec2(a.x * b.x - a.y * b.y,
                    a.x * b.y + a.y * b.x)
    end

    -- Smooth iteration count via the potential function.
    -- Returns a float in [0, max_iter].
    local function mandelbrot(c)
        local z = vec2(0.0, 0.0)
        local i = 0
        local escaped = 0.0
        while i < 128 do
            z = cmul(z, z) + c
            if dot(z, z) > 4.0 then
                -- smooth escape: nu = i - log2(log2(|z|))
                local log_zn = log(dot(z, z)) * 0.5
                local nu = log(log_zn / 0.6931) / 0.6931
                escaped = float(i) + 1.0 - nu
                i = 128  -- break
            end
            i = i + 1
        end
        return escaped
    end

    -- Palette: map iteration count to colour.
    local function colour(t)
        local r = 0.5 + 0.5 * cos(0.025 * t + 0.0)
        local g = 0.5 + 0.5 * cos(0.025 * t + 2.094)
        local b = 0.5 + 0.5 * cos(0.025 * t + 4.189)
        return vec3(r, g, b)
    end

    return function main(uv)
        local aspect = u_resolution.x / u_resolution.y

        -- Animated zoom toward a known interesting point.
        local zoom   = pow(0.5, u_time * 0.3)
        local centre = vec2(-0.7435, 0.1314)

        local c = vec2((uv.x - 0.5) * aspect, uv.y - 0.5) * zoom + centre

        local iter = mandelbrot(c)

        -- Inside the set → black; outside → palette colour.
        local inside = (iter >= 127.0) and 1.0 or 0.0
        local col = colour(iter) * (1.0 - inside)
        return vec4(col, 1.0)
    end
end)
