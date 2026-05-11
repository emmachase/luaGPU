-- noise_lib.lua
-- Reusable noise shaderlib: value noise, fBm, and a simple hash.
-- Demonstrates the shaderlib() API — only functions actually called by the
-- shader that imports this library are emitted into the final GLSL.

return shaderlib(function()

    -- ── Private helpers ────────────────────────────────────────────────────

    -- Classic 2-D hash: maps a vec2 lattice point to a pseudo-random float.
    local function hash2(p)
        local x = sin(dot(p, vec2(127.1, 311.7))) * 43758.5453
        return fract(x)
    end

    -- Smooth value noise: bilinear interpolation of hashed lattice values.
    local function value_noise(p)
        local i = vec2(floor(p.x), floor(p.y))
        local f = vec2(fract(p.x), fract(p.y))

        -- Smoothstep fade curve.
        local u = f * f * (vec2(3.0, 3.0) - 2.0 * f)

        local a = hash2(i)
        local b = hash2(i + vec2(1.0, 0.0))
        local c = hash2(i + vec2(0.0, 1.0))
        local d = hash2(i + vec2(1.0, 1.0))

        return mix(mix(a, b, u.x), mix(c, d, u.x), u.y)
    end

    -- ── Public API ─────────────────────────────────────────────────────────

    -- Fractional Brownian Motion: sum of value noise octaves.
    -- p    : 2-D input coordinates
    -- oct  : number of octaves (use a constant for GLSL compatibility)
    local function fbm(p, oct)
        local val   = 0.0
        local amp   = 0.5
        local freq  = 1.0
        local i     = 0
        while i < oct do
            val  = val + amp * value_noise(p * freq)
            amp  = amp  * 0.5
            freq = freq * 2.0
            i    = i + 1
        end
        return val
    end

    -- Direct access to value noise (public export).
    local function noise(p)
        return value_noise(p)
    end

    return { fbm = fbm, noise = noise }
end)
