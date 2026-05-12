-- raymarcher.lua
-- Minimal 3-D ray marcher: renders a scene composed of a ground plane,
-- a sphere and a torus using signed distance functions.
-- Lighting: single directional light with diffuse + ambient + soft shadow.

local myShader = shader(function(u_time, u_resolution)

    -- ── Named structs ─────────────────────────────────────────────────────
    local Hit = struct({ d = float, mat = float })

    -- ── SDF primitives ────────────────────────────────────────────────────

    local function sd_sphere(p, r)
        return length(p) - r
    end

    local function sd_plane(p)
        return p.y + 1.0
    end

    local function sd_torus(p, R, r)
        local q = vec2(length(vec2(p.x, p.z)) - R, p.y)
        return length(q) - r
    end

    local function smin(a, b, k)
        local h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
        return mix(b, a, h) - k * h * (1.0 - h)
    end

    -- ── Scene: returns Hit { d, mat }
    --   mat 0 = ground   1 = opaque objects   2 = glass sphere

    local function scene(p)
        local sphere_y = sin(u_time * 1.5) * 0.4
        local d_sphere = sd_sphere(p - vec3(0.0, sphere_y, 2.5), 0.6)

        local angle = u_time * 0.8
        local px =  cos(angle) * p.x + sin(angle) * p.z
        local pz = -sin(angle) * p.x + cos(angle) * p.z
        local d_torus = sd_torus(vec3(px, p.y, pz) - vec3(0.0, 0.0, 2.5), 1.0, 0.3)

        local d_ground  = sd_plane(p)
        local d_opaque  = smin(d_sphere, d_torus, 0.3)
        local d_glass   = sd_sphere(p - vec3(-1.6, 0.0, 2.2), 0.55)

        local d   = d_ground
        local mat = 0.0
        if d_opaque < d then d = d_opaque  mat = 1.0 end
        if d_glass  < d then d = d_glass   mat = 2.0 end
        return Hit({ d = d, mat = mat })
    end

    local function scene_dist(p)
        return scene(p).d
    end

    -- ── Ray march — returns Hit; h.d > 49 means sky ───────────────────────

    local function ray_march(ro, rd)
        local t   = 0.0
        local mat = 0.0
        local i   = 0
        while i < 96 do
            local h = scene(ro + rd * t)
            if h.d < 0.001 then
                mat = h.mat
                i = 96
            else
                t = t + h.d
                if t > 50.0 then i = 96 end
            end
            i = i + 1
        end
        return Hit({ d = t, mat = mat })
    end

    -- ── Normal (central differences) ──────────────────────────────────────

    local function calc_normal(p)
        local e = 0.001
        local nx = scene_dist(p + vec3(e, 0, 0)) - scene_dist(p - vec3(e, 0, 0))
        local ny = scene_dist(p + vec3(0, e, 0)) - scene_dist(p - vec3(0, e, 0))
        local nz = scene_dist(p + vec3(0, 0, e)) - scene_dist(p - vec3(0, 0, e))
        return normalize(vec3(nx, ny, nz))
    end

    -- ── Soft shadow ───────────────────────────────────────────────────────

    local function soft_shadow(ro, rd, tmax)
        local t   = 0.02
        local res = 1.0
        local i   = 0
        while i < 32 do
            local d = scene_dist(ro + rd * t)
            res = min(res, 8.0 * d / t)
            t   = t + clamp(d, 0.01, 0.2)
            if t > tmax then i = 32 end
            i = i + 1
        end
        return clamp(res, 0.0, 1.0)
    end

    -- ── Shade an opaque/ground hit ─────────────────────────────────────────

    local function shade_opaque(pos, n, mat, ld)
        local diff = clamp(dot(n, ld), 0.0, 1.0)
        local shad = soft_shadow(pos + n * 0.002, ld, 10.0)
        local amb  = 0.15 + 0.1 * n.y

        local ground_blend = clamp(pos.y + 1.05, 0.0, 1.0)
        local surf = mix(vec3(0.45, 0.48, 0.52), vec3(0.9, 0.5, 0.2), ground_blend)
        if mat < 0.5 then
            local cx = math.floor(pos.x * 2.0)
            local cz = math.floor(pos.z * 2.0)
            local checker = (cx + cz) % 2
            surf = mix(vec3(0.38, 0.4, 0.44), vec3(0.54, 0.56, 0.6), checker * 1.0)
        end
        return surf * (diff * shad + amb)
    end

    -- ── Shade a glass hit ─────────────────────────────────────────────────

    local function shade_glass(ro, rd, pos, n_outer, ld, sky_col)
        -- return ro
        local IOR        = 1.45
        local glass_tint = vec3(0.05, 0.55, 0.65)

        local cos_i   = clamp(-dot(rd, n_outer), 0.0, 1.0)
        local r0      = ((1.0 - IOR) / (1.0 + IOR))
        r0 = r0 * r0
        local fresnel = r0 + (1.0 - r0) * pow(1.0 - cos_i, 5.0)

        local refl_dir = rd - n_outer * (2.0 * dot(rd, n_outer))
        local rh       = ray_march(pos + n_outer * 0.003, refl_dir)
        local refl_col
        if rh.d > 49.0 then
            refl_col = sky_col
        else
            local rpos = pos + n_outer * 0.003 + refl_dir * rh.d
            local rn   = calc_normal(rpos)
            refl_col   = shade_opaque(rpos, rn, rh.mat, ld)
            refl_col   = pow(clamp(refl_col, 0.0, 1.0), vec3(0.4545))
        end

        local refr_dir = refract(rd, n_outer, 1.0 / IOR)
        local interior_t = 0.0
        local ii = 0
        while ii < 48 do
            local d = -sd_sphere((pos + refr_dir * interior_t) - vec3(-1.6, 0.0, 2.2), 0.55)
            d = max(d, 0.001)
            interior_t = interior_t + d
            if interior_t > 4.0 then ii = 48 end
            ii = ii + 1
        end

        local exit_pos = pos + refr_dir * interior_t
        local n_inner  = -calc_normal(exit_pos)
        local exit_dir = refract(refr_dir, n_inner, IOR)
        if length(exit_dir) < 0.1 then
            exit_dir = refr_dir
        end

        local absorption = exp(-glass_tint * interior_t * 2.5)

        local th       = ray_march(exit_pos + exit_dir * 0.003, exit_dir)
        local trans_col
        if th.d > 49.0 then
            trans_col = sky_col
        else
            local tpos = exit_pos + exit_dir * 0.003 + exit_dir * th.d
            local tn   = calc_normal(tpos)
            trans_col  = shade_opaque(tpos, tn, th.mat, ld)
            trans_col  = pow(clamp(trans_col, 0.0, 1.0), vec3(0.4545))
        end
        trans_col = trans_col * absorption

        return mix(trans_col, refl_col, fresnel)
    end

    -- ── Main entry ────────────────────────────────────────────────────────

    return function(uv)
        local aspect = u_resolution.x / u_resolution.y
        local ro     = vec3(sin(u_time * 0.2) * 3.0, 1.5, -1.0)
        local target = vec3(0.0, 0.0, 2.5)
        local fwd    = normalize(target - ro)
        local right  = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)))
        local up     = cross(right, fwd)

        local ndc = vec2((uv.x - 0.5) * aspect, uv.y - 0.5)
        local rd  = normalize(fwd + ndc.x * right + ndc.y * up)

        local h   = ray_march(ro, rd)
        local sky = vec3(0.4, 0.6, 0.9) * (1.0 - 0.4 * rd.y)
        local ld  = normalize(vec3(sin(u_time * 0.5), 1.5, cos(u_time * 0.5)))

        local col = vec3(0.0)
        if h.d > 49.0 then
            col = sky
        else
            local pos = ro + rd * h.d
            local n   = calc_normal(pos)
            if h.mat > 1.5 then
                col = shade_glass(ro, rd, pos, n, ld, sky)
            else
                col = shade_opaque(pos, n, h.mat, ld)
                col = pow(col, vec3(0.4545))
            end
        end

        -- local hn     = math.fract(math.sin(uv.x * 127.1 + uv.y * 311.7 + h.d) * 43758.5453)
        -- local dither = (hn - 0.5) * 0.08

        return vec4(col, 1.0)
    end
end)

return myShader
