-- fbm_clouds.lua
-- Animated cloud-like pattern using a shaderlib for fBm noise.
-- Demonstrates the shaderlib() API: noise_lib is captured as an upvalue of
-- the shader() closure; only the functions actually called (fbm) are emitted.

local noise = shaderlib(function()

    local function hash2(p)
        return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453)
    end

    local function value_noise(p)
        local i = vec2(floor(p.x), floor(p.y))
        local f = vec2(fract(p.x), fract(p.y))
        local u = f * f * (vec2(3.0, 3.0) - 2.0 * f)
        local a = hash2(i)
        local b = hash2(i + vec2(1.0, 0.0))
        local c = hash2(i + vec2(0.0, 1.0))
        local d = hash2(i + vec2(1.0, 1.0))
        return mix(mix(a, b, u.x), mix(c, d, u.x), u.y)
    end

    local function fbm(p, oct)
        local val  = 0.0
        local amp  = 0.5
        local freq = 1.0
        local i    = 0
        while i < oct do
            val  = val + amp * value_noise(p * freq)
            amp  = amp  * 0.5
            freq = freq * 2.0
            i    = i + 1
        end
        return val
    end

    return { fbm = fbm }
end)

local myShader = shader(function(u_time, u_resolution)
    return function(uv)
        -- Remap to aspect-correct space.
        local aspect = u_resolution.x / u_resolution.y
        local p = vec2((uv.x - 0.5) * aspect * 3.0,
                        (uv.y - 0.5) * 3.0)

        -- Animate by drifting the coordinates.
        local q = p + vec2(u_time * 0.12, u_time * 0.07)

        -- 6-octave fBm from the shaderlib.
        local n = noise.fbm(q, 6)

        -- Map noise to a blue-white sky palette.
        local sky   = vec3(0.4, 0.6, 0.9)
        local cloud = vec3(1.0, 1.0, 1.0)
        local col   = mix(sky, cloud, smoothstep(0.35, 0.75, n))

        return vec4(col, 1.0)
    end
end)

return myShader
