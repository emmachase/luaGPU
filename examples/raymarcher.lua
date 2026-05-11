-- raymarcher.lua
-- Minimal 3-D ray marcher: renders a scene composed of a ground plane,
-- a sphere and a torus using signed distance functions.
-- Lighting: single directional light with diffuse + ambient + soft shadow.

local myShader = shader(function(u_time, u_resolution)

    -- ── SDF primitives ────────────────────────────────────────────────────

    local function sd_sphere(p, r)
        return length(p) - r
    end

    local function sd_plane(p)
        -- infinite horizontal plane at y = -1
        return p.y + 1.0
    end

    -- Torus centred at origin in the XZ plane: R = major radius, r = tube radius
    local function sd_torus(p, R, r)
        local q = vec2(length(vec2(p.x, p.z)) - R, p.y)
        return length(q) - r
    end

    -- Smooth union (k controls blend width)
    local function smin(a, b, k)
        local h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
        return mix(b, a, h) - k * h * (1.0 - h)
    end

    -- ── Scene ─────────────────────────────────────────────────────────────

    local function scene(p)
        -- Animate: sphere bobs up and down, torus spins.
        local sphere_y = sin(u_time * 1.5) * 0.4
        local d_sphere = sd_sphere(p - vec3(0.0, sphere_y, 2.5), 0.6)

        -- Rotate torus around Y axis.
        local angle = u_time * 0.8
        local px =  cos(angle) * p.x + sin(angle) * p.z
        local pz = -sin(angle) * p.x + cos(angle) * p.z
        local d_torus = sd_torus(vec3(px, p.y, pz) - vec3(0.0, 0.0, 2.5),
                                  1.0, 0.3)

        local d_ground = sd_plane(p)

        local d_combined = smin(d_sphere, d_torus, 0.3)
        return min(d_combined, d_ground)
    end

    -- ── Ray march ─────────────────────────────────────────────────────────

    local function ray_march(ro, rd)
        local t = 0.0
        local i = 0
        while i < 96 do
            local d = scene(ro + rd * t)
            if d < 0.001 then
                i = 96  -- break
            end
            t = t + d
            if t > 50.0 then
                i = 96  -- break
            end
            i = i + 1
        end
        return t
    end

    -- ── Normal estimation by central differences ──────────────────────────

    local function normal(p)
        local e = 0.001
        local nx = scene(p + vec3(e, 0.0, 0.0)) - scene(p - vec3(e, 0.0, 0.0))
        local ny = scene(p + vec3(0.0, e, 0.0)) - scene(p - vec3(0.0, e, 0.0))
        local nz = scene(p + vec3(0.0, 0.0, e)) - scene(p - vec3(0.0, 0.0, e))
        return normalize(vec3(nx, ny, nz))
    end

    -- ── Soft shadow ───────────────────────────────────────────────────────

    local function soft_shadow(ro, rd, tmax)
        local t   = 0.02
        local res = 1.0
        local i   = 0
        while i < 32 do
            local d = scene(ro + rd * t)
            res = min(res, 8.0 * d / t)
            t   = t + clamp(d, 0.01, 0.2)
            if t > tmax then
                i = 32  -- break
            end
            i = i + 1
        end
        return clamp(res, 0.0, 1.0)
    end

    -- ── Shading ───────────────────────────────────────────────────────────

    return function(uv)
        -- Camera: look toward the scene centre from a slightly elevated position.
        local aspect = u_resolution.x / u_resolution.y
        local ro = vec3(sin(u_time * 0.2) * 3.0, 1.5, -1.0)
        local target = vec3(0.0, 0.0, 2.5)
        local fwd  = normalize(target - ro)
        local right = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)))
        local up   = cross(right, fwd)

        -- Screen-space ray direction.
        local ndc = vec2((uv.x - 0.5) * aspect, uv.y - 0.5)
        local rd  = normalize(fwd + ndc.x * right + ndc.y * up)

        local t = ray_march(ro, rd)

        local sky = vec3(0.4, 0.6, 0.9) * (1.0 - 0.4 * rd.y)

        if t > 49.0 then
            return vec4(sky, 1.0)
        end

        local pos = ro + rd * t
        local n   = normal(pos)

        local x = ivec2(3) + 1

        -- Light direction (animated).
        local ld = normalize(vec3(sin(u_time * 0.5), 1.5, cos(u_time * 0.5)))

        local diff = clamp(dot(n, ld), 0.0, 1.0)
        local shad = soft_shadow(pos + n * 0.002, ld, 10.0)
        local amb  = 0.15 + 0.1 * n.y

        -- Simple surface colour: ground is grey, objects are warm orange.
        local ground_blend = clamp(pos.y + 1.05, 0.0, 1.0)
        local surf = mix(vec3(0.6, 0.55, 0.5), vec3(0.9, 0.5, 0.2), ground_blend)

        local col = surf * (diff * shad + amb)

        -- Gamma correction.
        col = pow(clamp(col, 0.0, 1.0), 0.4545)
        return vec4(col, 1.0)
    end
end)

return myShader
