-- octgrams.lua
-- Animated ray-marched tunnel of overlapping SDF boxes ("Octgrams").
-- Original GLSL by @kishimisu / Shadertoy. Ported to luaGPU Lua dialect.
--
-- The original shader uses a mutable global `gTime` that is updated inside
-- the ray-march loop to offset the animation per-step.  In this port `gTime`
-- is threaded as an explicit parameter through every function that needs it.

local myShader = shader(function(u_time, u_resolution)

    -- 2-D rotation matrix.
    local function rot(a)
        local c = cos(a)
        local s = sin(a)
        return mat2(c, s, -s, c)
    end

    -- Signed distance to an axis-aligned box centred at the origin.
    local function sd_box(p, b)
        local q = abs(p) - b
        return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0)
    end

    -- A single hollow-box primitive.
    --   pos   : sample position (already scaled by `scale`)
    --   scale : uniform scale factor applied to pos before the SDF
    local function box(pos, scale)
        pos = pos * scale
        local base = sd_box(pos, vec3(0.4, 0.4, 0.1)) / 1.5
        -- Carve a slot along the rotated y-axis.
        local pxy = vec2(pos.x, pos.y) * 5.0
        pxy = pxy - vec2(0.0, 3.5)
        pxy = rot(0.75) * pxy
        return -base
    end

    -- Six boxes combined with max() to form one "octagram" cross-section.
    --   g_time : per-step time offset (replaces the mutable global in the
    --            original shader)
    local function box_set(pos, g_time)
        local origin = pos

        local animated_scale = 2.0 - abs(sin(g_time * 0.4)) * 1.5
        local rxy = rot(0.8)

        -- box 1 – shifted +Y
        local p1 = vec3(origin.x, origin.y + sin(g_time * 0.4) * 2.5, origin.z)
        local p1xy = rxy * vec2(p1.x, p1.y)
        local b1 = box(vec3(p1xy.x, p1xy.y, p1.z), animated_scale)

        -- box 2 – shifted -Y
        local p2 = vec3(origin.x, origin.y - sin(g_time * 0.4) * 2.5, origin.z)
        local p2xy = rxy * vec2(p2.x, p2.y)
        local b2 = box(vec3(p2xy.x, p2xy.y, p2.z), animated_scale)

        -- box 3 – shifted +X
        local p3 = vec3(origin.x + sin(g_time * 0.4) * 2.5, origin.y, origin.z)
        local p3xy = rxy * vec2(p3.x, p3.y)
        local b3 = box(vec3(p3xy.x, p3xy.y, p3.z), animated_scale)

        -- box 4 – shifted -X
        local p4 = vec3(origin.x - sin(g_time * 0.4) * 2.5, origin.y, origin.z)
        local p4xy = rxy * vec2(p4.x, p4.y)
        local b4 = box(vec3(p4xy.x, p4xy.y, p4.z), animated_scale)

        -- box 5 – centred, rotated, small scale * 6
        local p5xy = rxy * vec2(origin.x, origin.y)
        local b5 = box(vec3(p5xy.x, p5xy.y, origin.z), 0.5) * 6.0

        -- box 6 – centred, unrotated, small scale * 6
        local b6 = box(origin, 0.5) * 6.0

        return max(max(max(max(max(b1, b2), b3), b4), b5), b6)
    end

    -- ── Main entry ────────────────────────────────────────────────────────────

    return function(uv)
        -- Reconstruct the Shadertoy-style -1…1 NDC from the [0,1] uv.
        local aspect = u_resolution.x / u_resolution.y
        local mn     = min(u_resolution.x, u_resolution.y)
        local p      = (uv * 2.0 - vec2(1.0, 1.0)) * vec2(u_resolution.x, u_resolution.y) / mn

        -- Camera: moves forward along Z over time.
        local ro  = vec3(0.0, -0.2, u_time * 4.0)
        local ray = normalize(vec3(p.x, p.y, 1.5))

        -- Animate camera roll (xy) and pitch (yz).
        local rxy = rot(sin(u_time * 0.03) * 5.0)
        local ryz = rot(sin(u_time * 0.05) * 0.2)

        local ray_xy = rxy * vec2(ray.x, ray.y)
        ray = vec3(ray_xy.x, ray_xy.y, ray.z)

        local ray_yz = ryz * vec2(ray.y, ray.z)
        ray = vec3(ray.x, ray_yz.x, ray_yz.y)

        -- Ray march loop (99 steps, accumulate glow).
        local t  = 0.1
        local ac = 0.0
        local i  = 0
        while i < 99 do
            local pos = ro + ray * t

            -- Tile space: repeat every 4 units, centred on -2…2.
            pos = mod(pos - vec3(2.0, 2.0, 2.0), vec3(4.0, 4.0, 4.0)) - vec3(2.0, 2.0, 2.0)

            -- Per-step time offset (replicates the original mutable gTime).
            local g_time = u_time - float(i) * 0.01

            local d = box_set(pos, g_time)
            d  = max(abs(d), 0.01)
            ac = ac + exp(-d * 23.0)
            t  = t  + d * 0.55
            i  = i  + 1
        end

        -- Compose final colour.
        local col = vec3(ac * 0.02, ac * 0.02, ac * 0.02)
        col = col + vec3(0.0,
                         0.2 * abs(sin(u_time)),
                         0.5 + sin(u_time) * 0.2)

        local alpha = 1.0 - t * (0.02 + 0.02 * sin(u_time))
        return vec4(col*alpha, 1.0)
    end
end)

return myShader
