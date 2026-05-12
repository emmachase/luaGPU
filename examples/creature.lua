-- creature.lua
-- Port of Inigo Quilez's "Creature" shader (2019).
-- Original: https://iquilezles.org/
-- Copyright Inigo Quilez, 2019. Educational use / linking only.
-- Ported to luaGPU Lua dialect: AA removed (single sample per pixel),
-- mutable globals (href, hsha) threaded through map() return struct.

local myShader = shader(function(u_time, u_resolution)

    -- ── Named structs ─────────────────────────────────────────────────────────

    -- map() return: SDF result vec4 + the two "side-channel" scalars.
    -- res components: x=dist, y=matID, z=extra, w=focc
    local MapResult = struct({ res = vec4, href = float, hsha = float })

    -- ── SDF helpers ───────────────────────────────────────────────────────────

    local function smin_f(a, b, k)
        local h = max(k - abs(a - b), 0.0)
        return min(a, b) - h * h * 0.25 / k
    end

    local function smin_v2(a, b, k)
        local h = clamp(0.5 + 0.5 * (b.x - a.x) / k, 0.0, 1.0)
        return mix(b, a, h) - vec2(k, k) * h * (1.0 - h)
    end

    local function smax_f(a, b, k)
        local h = max(k - abs(a - b), 0.0)
        return max(a, b) + h * h * 0.25 / k
    end

    local function sd_sphere(p, s)
        return length(p) - s
    end

    local function sd_ellipsoid(p, r)
        local k0 = length(p / r)
        local k1 = length(p / (r * r))
        return k0 * (k0 - 1.0) / k1
    end

    -- Returns vec2(dist, h) where h is the stick blend parameter.
    local function sd_stick(p, a, b, r1, r2)
        local pa = p - a
        local ba = b - a
        local h  = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0)
        local sm = h * h * (3.0 - 2.0 * h)
        return vec2(length(pa - ba * h) - mix(r1, r2, sm), h)
    end

    -- Select whichever vec4 has the smaller x component.
    local function op_u(d1, d2)
        if d1.x < d2.x then
            return d1
        end
        return d2
    end

    -- ── Scene SDF ─────────────────────────────────────────────────────────────
    -- Returns MapResult.  All GLSL mutable-global side-effects are captured
    -- in the returned href and hsha fields.

    local function map(pos, atime)
        local hsha = 1.0

        local t1 = math.fract(atime)
        local t4 = abs(math.fract(atime * 0.5) - 0.5) / 0.5

        local p   = 4.0 * t1 * (1.0 - t1)
        local pp  = 4.0 * (1.0 - 2.0 * t1)   -- derivative of p

        local cen = vec3(0.5 * (-1.0 + 2.0 * t4),
                         pow(p, 2.0 - p) + 0.1,
                         math.floor(atime) + pow(t1, 0.7) - 1.0)

        -- body orientation
        local uu_raw = normalize(vec2(1.0, -pp))
        local uu = uu_raw
        local vv = vec2(-uu.y, uu.x)

        local sy       = 0.5 + 0.5 * p
        local compress = 1.0 - smoothstep(0.0, 0.4, p)
        sy = sy * (1.0 - compress) + compress
        local sz = 1.0 / sy

        local q   = pos - cen
        local rot = -0.25 * (-1.0 + 2.0 * t4)
        local rc  = cos(rot)
        local rs  = sin(rot)
        local qxy = mat2(rc, rs, -rs, rc) * vec2(q.x, q.y)
        q = vec3(qxy.x, qxy.y, q.z)

        local r    = q
        local href = q.y

        local qyz = vec2(dot(uu, vec2(q.y, q.z)), dot(vv, vec2(q.y, q.z)))
        q = vec3(q.x, qyz.x, qyz.y)

        local deli = sd_ellipsoid(q, vec3(0.25, 0.25 * sy, 0.25 * sz))
        local res  = vec4(deli, 2.0, 0.0, 1.0)

        -- ground + ripple
        local fh   = -0.1 - 0.05 * (sin(pos.x * 2.0) + sin(pos.z * 2.0))
        local t5f  = math.fract(atime + 0.05)
        local t5i  = math.floor(atime + 0.05)
        local bt4  = abs(math.fract(t5i * 0.5) - 0.5) / 0.5
        local bcen = vec2(0.5 * (-1.0 + 2.0 * bt4), t5i + pow(t5f, 0.7) - 1.0)

        local k  = length(vec2(pos.x, pos.z) - bcen)
        local tt = t5f * 15.0 - 6.2831 - k * 3.0
        fh = fh - 0.1 * exp(-k * k) * sin(tt) * exp(-max(tt, 0.0) / 2.0)
                    * smoothstep(0.0, 0.01, t5f)
        local d = pos.y - fh

        -- bubbles
        local vp  = vec3(mod(abs(pos.x), 3.0) - 1.5,
                         pos.y,
                         mod(pos.z + 1.5, 3.0) - 1.5)
        local bid = vec2(math.floor(pos.x / 3.0),
                         math.floor((pos.z + 1.5) / 3.0))
        local fid  = bid.x * 11.1 + bid.y * 31.7
        local fy   = math.fract(fid * 1.312 + atime * 0.1)
        local by   = -1.0 + 4.0 * fy
        local brad = vec3(0.7, 1.0 + 0.5 * sin(fid), 0.7)
        brad = brad - 0.1 * (sin(pos.x * 3.0) + sin(pos.y * 4.0) + sin(pos.z * 5.0))
        local siz  = 4.0 * fy * (1.0 - fy)
        local d2   = sd_ellipsoid(vp - vec3(0.5, by, 0.0), siz * brad)
        d2 = d2 - 0.03 * smoothstep(-1.0, 1.0,
                            sin(18.0 * pos.x) + sin(18.0 * pos.y) + sin(18.0 * pos.z))
        d2 = d2 * 0.6
        d2 = min(d2, 2.0)
        d  = smin_f(d, d2, 0.32)
        if d < res.x then
            res  = vec4(d, 1.0, 0.0, 1.0)
            hsha = sqrt(siz)
        end

        -- rest of body (guarded by bounding volume)
        if deli - 1.0 < res.x then
            local t2 = math.fract(atime + 0.8)
            local p2 = 0.5 - 0.5 * cos(6.2831 * t2)
            local rr = vec3(r.x, r.y + 0.2 * sy - 0.2, r.z + 0.05 - 0.2 * p2)
            local sq = vec3(abs(rr.x), rr.y, rr.z)

            -- head
            local h_vec = rr
            local hr    = sin(0.791 * atime)
            hr = 0.7 * sign(hr) * smoothstep(0.5, 0.7, abs(hr))
            local hxz   = mat2(cos(hr), sin(hr), -sin(hr), cos(hr)) * vec2(h_vec.x, h_vec.z)
            h_vec = vec3(hxz.x, h_vec.y, hxz.y)
            local hq    = vec3(abs(h_vec.x), h_vec.y, h_vec.z)

            local dh  = sd_ellipsoid(h_vec - vec3(0.0, 0.20, 0.02), vec3(0.08, 0.2, 0.15))
            local dh2 = sd_sphere(h_vec - vec3(0.0, 0.21, -0.1), 0.2)
            dh = smin_f(dh, dh2, 0.1)
            res = vec4(smin_f(res.x, dh, 0.1), res.y, res.z, res.w)

            -- belly wrinkles
            local yy = rr.y - 0.02 - 2.5 * rr.x * rr.x
            local rx  = res.x + 0.001 * sin(yy * 120.0)
                            * (1.0 - smoothstep(0.0, 0.1, abs(yy)))
            res = vec4(rx, res.y, res.z, res.w)

            -- arms
            local arm_stick = sd_stick(sq,
                vec3(0.18 - 0.06 * hr * sign(rr.x), 0.2, -0.05),
                vec3(0.3 + 0.1 * p2, -0.2 + 0.3 * p2, -0.15),
                0.03, 0.06)
            local arm_k  = 0.01 + 0.04 * pow(1.0 - arm_stick.y, 3.0)
            local arm_xz = smin_v2(vec2(res.x, res.z), arm_stick, arm_k)
            res = vec4(arm_xz.x, res.y, arm_xz.y, res.w)

            -- ears
            local t3  = math.fract(atime + 0.9)
            local p3  = 4.0 * t3 * (1.0 - t3)
            local ear = sd_stick(hq,
                vec3(0.15, 0.32, -0.05),
                vec3(0.2 + 0.05 * p3, 0.2 + 0.2 * p3, -0.07),
                0.01, 0.04)
            local ear_xz = smin_v2(vec2(res.x, res.z), ear, 0.01)
            res = vec4(ear_xz.x, res.y, ear_xz.y, res.w)

            -- mouth (carve with smax)
            local dm = sd_ellipsoid(h_vec - vec3(0.0, 0.15 + 4.0 * hq.x * hq.x, 0.15),
                                    vec3(0.1, 0.04, 0.2))
            local new_w = 0.3 + 0.7 * clamp(dm * 150.0, 0.0, 1.0)
            local new_x = smax_f(res.x, -dm, 0.03)
            res = vec4(new_x, res.y, res.z, new_w)

            -- legs
            local t6   = cos(6.2831 * (atime * 0.5 + 0.25))
            local ccc  = cos(1.57 * t6 * sign(rr.x))
            local sss  = sin(1.57 * t6 * sign(rr.x))
            local base = vec3(0.12, -0.07 - 0.1 / sy, -0.1)
            local leg  = sd_stick(sq, base,
                base + vec3(0.2, -ccc, sss) * 0.2, 0.04, 0.07)
            local leg_xz = smin_v2(vec2(res.x, res.z), leg, 0.07)
            res = vec4(leg_xz.x, res.y, leg_xz.y, res.w)

            -- eye
            local blink   = pow(0.5 + 0.5 * sin(2.1 * u_time), 20.0)
            local eyeball = sd_sphere(hq - vec3(0.08, 0.27, 0.06), 0.065 + 0.02 * blink)
            res = vec4(smin_f(res.x, eyeball, 0.03), res.y, res.z, res.w)

            local cq   = hq - vec3(0.1, 0.34, 0.08)
            local cqxy = mat2(0.8, 0.6, -0.6, 0.8) * vec2(cq.x, cq.y)
            cq = vec3(cqxy.x, cqxy.y, cq.z)
            local de   = sd_ellipsoid(cq, vec3(0.06, 0.03, 0.03))
            res = vec4(smin_f(res.x, de, 0.03), res.y, res.z, res.w)

            local eo  = 1.0 - 0.5 * smoothstep(0.01, 0.04,
                            length((vec2(hq.x, hq.y) - vec2(0.095, 0.285))
                                   * vec2(1.0, 1.1)))
            res = op_u(res, vec4(sd_sphere(hq - vec3(0.08, 0.28, 0.08),  0.060), 3.0, 0.0, eo))
            res = op_u(res, vec4(sd_sphere(hq - vec3(0.075, 0.28, 0.102), 0.0395), 4.0, 0.0, 1.0))
        end

        -- candy (guarded by bounding volume)
        if pos.y - 1.0 < res.x then
            local fs  = 5.0
            local qos = vec3(pos.x, pos.y - fh, pos.z) * fs
            local cid = vec2(math.floor(qos.x + 0.5), math.floor(qos.z + 0.5))
            local cvp = vec3(math.fract(qos.x + 0.5) - 0.5, qos.y,
                             math.fract(qos.z + 0.5) - 0.5)
            cvp = vec3(cvp.x + 0.1 * cos(cid.x * 130.143 + cid.y * 120.372),
                       cvp.y,
                       cvp.z + 0.1 * cos(cid.x * 130.143 + cid.y * 120.372 + 2.0))
            local den = sin(cid.x * 0.1 + sin(cid.y * 0.091)) + sin(cid.y * 0.1)
            local cfid = cid.x * 0.143 + cid.y * 0.372
            local ra  = smoothstep(0.0, 0.1, den * 0.1 + math.fract(cfid) - 0.95)
            local dc  = sd_sphere(cvp, 0.35 * ra) / fs
            if dc < res.x then
                res = vec4(dc, 5.0, qos.y, 1.0)
            end
        end

        return MapResult({ res = res, href = href, hsha = hsha })
    end

    -- ── Raycast ───────────────────────────────────────────────────────────────

    local function raycast(ro, rd, time)
        local result = vec4(-1.0, -1.0, 0.0, 1.0)
        local tmin   = 0.5
        local tmax   = 20.0

        -- clip to bounding plane y=3.4
        local tp = (3.4 - ro.y) / rd.y
        if tp > 0.0 then
            tmax = min(tmax, tp)
        end

        local t = tmin
        local i = 0
        while i < 256 do
            local mr = map(ro + rd * t, time)
            local h  = mr.res
            if abs(h.x) < 0.0005 * t then
                result = vec4(t, h.y, h.z, h.w)
                i = 256
            else
                t = t + h.x
                if t >= tmax then
                    i = 256
                end
            end
            i = i + 1
        end
        return result
    end

    -- ── Soft shadow ───────────────────────────────────────────────────────────

    local function calc_soft_shadow(ro, rd, time)
        local res  = 1.0
        local tmax = 12.0

        local tp = (3.4 - ro.y) / rd.y
        if tp > 0.0 then
            tmax = min(tmax, tp)
        end

        local t = 0.02
        local i = 0
        while i < 50 do
            local mr   = map(ro + rd * t, time)
            local h    = mr.res.x
            local hsha = mr.hsha
            res = min(res, mix(1.0, 16.0 * h / t, hsha))
            t   = t + clamp(h, 0.05, 0.40)
            if res < 0.005 then
                i = 50
            end
            if t > tmax then
                i = 50
            end
            i = i + 1
        end
        return clamp(res, 0.0, 1.0)
    end

    -- ── Normal ────────────────────────────────────────────────────────────────

    local function calc_normal(pos, time)
        local e = 0.001
        local nx = map(pos + vec3(e, 0.0, 0.0), time).res.x
                 - map(pos - vec3(e, 0.0, 0.0), time).res.x
        local ny = map(pos + vec3(0.0, e, 0.0), time).res.x
                 - map(pos - vec3(0.0, e, 0.0), time).res.x
        local nz = map(pos + vec3(0.0, 0.0, e), time).res.x
                 - map(pos - vec3(0.0, 0.0, e), time).res.x
        return normalize(vec3(nx, ny, nz))
    end

    -- ── Occlusion ─────────────────────────────────────────────────────────────

    local function calc_occlusion(pos, nor, time)
        local occ = 0.0
        local sca = 1.0
        local i   = 0
        while i < 5 do
            local h    = 0.01 + 0.11 * float(i) / 4.0
            local opos = pos + nor * h
            local d    = map(opos, time).res.x
            occ = occ + (h - d) * sca
            sca = sca * 0.95
            i   = i + 1
        end
        return clamp(1.0 - 2.0 * occ, 0.0, 1.0)
    end

    -- ── Render ────────────────────────────────────────────────────────────────

    local function render(ro, rd, time)
        -- sky dome
        local col = vec3(0.5, 0.8, 0.9) - max(rd.y, 0.0) * 0.5
        -- sky clouds
        local suv = vec2(rd.x, rd.z) * 1.5 / rd.y
        local cl  = 1.0 * (sin(suv.x) + sin(suv.y))
        local suv2 = mat2(0.8, 0.6, -0.6, 0.8) * suv * 2.1
        cl = cl + 0.5 * (sin(suv2.x) + sin(suv2.y))
        col = col + 0.1 * (-1.0 + 2.0 * smoothstep(-0.1, 0.1, cl - 0.4))
        -- sky horizon
        col = mix(col, vec3(0.5, 0.7, 0.9), exp(-10.0 * max(rd.y, 0.0)))

        local res = raycast(ro, rd, time)
        if res.y > -0.5 then
            local t    = res.x
            local pos  = ro + rd * t
            local nor  = calc_normal(pos, time)
            local ref  = reflect(rd, nor)
            local focc = res.w

            -- re-run map at hit point to get href for body colouring
            local mr   = map(pos, time)
            local href = mr.href

            -- material
            col = vec3(0.2, 0.2, 0.2)
            local ks = 1.0

            if res.y > 4.5 then
                -- candy
                col = vec3(0.14, 0.048, 0.0)
                local cid = math.floor(vec2(pos.x, pos.z) * 5.0 + vec2(0.5, 0.5))
                local ch  = cid.x * 11.1 + cid.y * 37.341
                col = col + 0.036 * cos(vec3(ch, ch + 1.0, ch + 2.0))
                col = max(col, 0.0)
                focc = clamp(4.0 * res.z, 0.0, 1.0)
            elseif res.y > 3.5 then
                -- pupil
                col = vec3(0.0, 0.0, 0.0)
            elseif res.y > 2.5 then
                -- iris
                col = vec3(0.4, 0.4, 0.4)
            elseif res.y > 1.5 then
                -- body
                col = mix(vec3(0.144, 0.09, 0.0036), vec3(0.36, 0.1, 0.04),
                          res.z * res.z)
                col = mix(col, vec3(0.28, 0.18, 0.12),
                          (1.0 - res.z) * smoothstep(-0.15, 0.15, -href))
            else
                -- terrain
                col = vec3(0.05, 0.09, 0.02)
                local f = 0.2 * (-1.0 + 2.0 * smoothstep(-0.2, 0.2,
                              sin(18.0 * pos.x) + sin(18.0 * pos.y) + sin(18.0 * pos.z)))
                col = col + f * vec3(0.06, 0.06, 0.02)
                ks  = 0.5 + pos.y * 0.15

                -- footprints
                local mp   = vec2(pos.x - 0.5 * (mod(math.floor(pos.z + 0.5), 2.0) * 2.0 - 1.0),
                                  math.fract(pos.z + 0.5) - 0.5)
                local mark = 1.0 - smoothstep(0.1, 0.5, length(mp))
                mark = mark * smoothstep(0.0, 0.1,
                           math.floor(time) - math.floor(pos.z + 0.5))
                col = col * mix(vec3(1.0, 1.0, 1.0), vec3(0.5, 0.5, 0.4), mark)
                ks  = ks * (1.0 - 0.5 * mark)
            end

            -- lighting
            local occ     = calc_occlusion(pos, nor, time) * focc
            local fre     = clamp(1.0 + dot(nor, rd), 0.0, 1.0)

            local sun_lig = normalize(vec3(0.6, 0.35, 0.5))
            local sun_dif = clamp(dot(nor, sun_lig), 0.0, 1.0)
            local sun_hal = normalize(sun_lig - rd)
            local sun_sha = calc_soft_shadow(pos, sun_lig, time)
            local sun_spe = ks * pow(clamp(dot(nor, sun_hal), 0.0, 1.0), 8.0)
                          * sun_dif
                          * (0.04 + 0.96 * pow(clamp(1.0 + dot(sun_hal, rd), 0.0, 1.0), 5.0))
            local sky_dif = sqrt(clamp(0.5 + 0.5 * nor.y, 0.0, 1.0))
            local sky_spe = ks * smoothstep(0.0, 0.5, ref.y)
                          * (0.04 + 0.96 * pow(fre, 4.0))
            local bou_dif = sqrt(clamp(0.1 - 0.9 * nor.y, 0.0, 1.0))
                          * clamp(1.0 - 0.1 * pos.y, 0.0, 1.0)
            local bac_dif = clamp(0.1 + 0.9 * dot(nor,
                                normalize(vec3(-sun_lig.x, 0.0, -sun_lig.z))),
                                0.0, 1.0)
            local sss_dif = fre * sky_dif * (0.25 + 0.75 * sun_dif * sun_sha)

            local lin = vec3(0.0, 0.0, 0.0)
            lin = lin + sun_dif * vec3(8.10, 6.00, 4.20)
                      * vec3(sun_sha,
                             sun_sha * sun_sha * 0.5 + 0.5 * sun_sha,
                             sun_sha * sun_sha)
            lin = lin + sky_dif * vec3(0.50, 0.70, 1.00) * occ
            lin = lin + bou_dif * vec3(0.20, 0.70, 0.10) * occ
            lin = lin + bac_dif * vec3(0.45, 0.35, 0.25) * occ
            lin = lin + sss_dif * vec3(3.25, 2.75, 2.50) * occ
            col = col * lin
            col = col + sun_spe * vec3(9.90, 8.10, 6.30) * sun_sha
            col = col + sky_spe * vec3(0.20, 0.30, 0.65) * occ * occ

            col = pow(col, vec3(0.8, 0.9, 1.0))

            -- fog
            col = mix(col, vec3(0.5, 0.7, 0.9), 1.0 - exp(-0.0001 * t * t * t))
        end

        return col
    end

    -- ── Camera helper ─────────────────────────────────────────────────────────

    local function set_camera(ro, ta, cr)
        local cw = normalize(ta - ro)
        local cp = vec3(sin(cr), cos(cr), 0.0)
        local cu = normalize(cross(cw, cp))
        local cv = cross(cu, cw)
        return mat3(cu.x, cv.x, cw.x,
                    cu.y, cv.y, cw.y,
                    cu.z, cv.z, cw.z)
    end

    -- ── Main entry ────────────────────────────────────────────────────────────

    return function(uv)
        local time = u_time
        time = time - 2.6
        time = time * 0.9

        -- screen-space NDC matching the original (uses resolution height)
        local aspect = u_resolution.x / u_resolution.y
        local p = (uv * 2.0 - vec2(1.0, 1.0)) * vec2(aspect, 1.0)

        -- camera
        local cl = sin(0.5 * time)
        local an = 1.57 + 0.7 * sin(0.15 * time)
        local ta = vec3(0.0, 0.65, -0.6 + time * 1.0 - 0.4 * cl)
        local ro = ta + vec3(1.3 * cos(an), -0.250, 1.3 * sin(an))

        local ti = math.fract(time - 0.15)
        ti = 4.0 * ti * (1.0 - ti)
        local ta_y = ta.y + 0.15 * ti * ti * (3.0 - 2.0 * ti)
                          * smoothstep(0.4, 0.9, cl)
        ta = vec3(ta.x, ta_y, ta.z)

        -- camera bounce on landing
        local t4b  = abs(math.fract(time * 0.5) - 0.5) / 0.5
        local bou  = -1.0 + 2.0 * t4b
        local bump = 0.06 * sin(time * 12.0 + vec3(0.0, 2.0, 4.0))
                   * smoothstep(0.85, 1.0, abs(bou))
        ro = ro + bump

        local ca = set_camera(ro, ta, 0.0)
        local rd = ca * normalize(vec3(p.x, p.y, 1.8))

        local col = render(ro, rd, time)

        -- colour grading
        col = col * vec3(1.11, 0.89, 0.79)
        -- compress
        col = 1.35 * col / (1.0 + col)
        -- gamma
        col = pow(col, vec3(0.4545, 0.4545, 0.4545))
        -- s-curve
        col = clamp(col, 0.0, 1.0)
        col = col * col * (3.0 - 2.0 * col)
        -- vignette
        local vig = 0.5 + 0.5 * pow(16.0 * uv.x * uv.y
                                        * (1.0 - uv.x) * (1.0 - uv.y), 0.25)
        col = col * vig

        return vec4(col, 1.0)
    end
end)

return myShader
