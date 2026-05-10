-- struct_test.lua
-- Exercises anonymous structs:
--   Ray { origin: vec3, dir: vec3 }
--   Hit { t: float, normal: vec3, colour: vec3 }
-- Traces a simple sphere + ground plane, no external libraries.

local myShader = shader(function(u_time, u_resolution)

    -- Build a camera ray from UV.
    local function make_ray(uv, aspect)
        local ro = vec3(sin(u_time * 0.4) * 3.0, 1.2, cos(u_time * 0.4) * 3.0)
        local target = vec3(0.0, 0.0, 0.0)
        local fwd    = normalize(target - ro)
        local right  = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)))
        local up     = cross(right, fwd)
        local d = normalize(fwd + uv.x * aspect * right + uv.y * up)
        return { origin = ro, dir = d }
    end

    -- Intersect a sphere; returns { t, normal, colour }.
    -- t < 0 means miss.
    local function hit_sphere(ray, centre, radius, col)
        local oc = ray.origin - centre
        local b  = dot(oc, ray.dir)
        local c  = dot(oc, oc) - radius * radius
        local disc = b * b - c
        if disc < 0.0 then
            return { t = -1.0, normal = vec3(0.0), colour = col }
        end
        local sq = sqrt(disc)
        local t  = -b - sq
        if t < 0.001 then
            t = -b + sq
        end
        if t < 0.001 then
            return { t = -1.0, normal = vec3(0.0), colour = col }
        end
        local pos = ray.origin + ray.dir * t
        local n   = normalize(pos - centre)
        return { t = t, normal = n, colour = col }
    end

    -- Ground plane at y = -1; returns { t, normal, colour }.
    local function hit_plane(ray)
        local denom = ray.dir.y
        if abs(denom) < 0.0001 then
            return { t = -1.0, normal = vec3(0.0, 1.0, 0.0), colour = vec3(0.5) }
        end
        local tt = (-1.0 - ray.origin.y) / denom
        if tt < 0.001 then
            return { t = -1.0, normal = vec3(0.0, 1.0, 0.0), colour = vec3(0.5) }
        end
        -- Checkerboard
        local pos = ray.origin + ray.dir * tt
        local cx  = floor(pos.x)
        local cz  = floor(pos.z)
        local check = mod(cx + cz, 2.0)
        local light = vec3(0.9, 0.9, 0.9)
        local dark  = vec3(0.3, 0.3, 0.3)
        local c = (check > 0.5) and light or dark
        return { t = tt, normal = vec3(0.0, 1.0, 0.0), colour = c }
    end

    -- Pick the closer of two hits (both share the same struct shape).
    local function closer(a, b)
        if a.t < 0.0 then return b end
        if b.t < 0.0 then return a end
        if a.t < b.t then return a end
        return b
    end

    return function(uv)
        local aspect = u_resolution.x / u_resolution.y
        -- Remap uv from [0,1] to [-0.5, 0.5]
        local suv = uv - vec2(0.5, 0.5)

        local ray = make_ray(suv, aspect)

        -- Three spheres
        local h1 = hit_sphere(ray, vec3(0.0, 0.0, 0.0),   0.6,  vec3(0.9, 0.3, 0.2))
        local h2 = hit_sphere(ray, vec3(1.2, 0.0, 0.5),   0.4,  vec3(0.2, 0.6, 0.9))
        local h3 = hit_sphere(ray, vec3(-1.0, 0.0, -0.3), 0.5,  vec3(0.3, 0.9, 0.4))
        local hp = hit_plane(ray)

        local hit = closer(closer(closer(h1, h2), h3), hp)

        -- Sky
        local sky = vec3(0.4, 0.6, 1.0) * (0.7 + 0.3 * ray.dir.y)
        if hit.t < 0.0 then
            return vec4(sky, 1.0)
        end

        -- Simple diffuse lighting
        local ld   = normalize(vec3(0.8, 1.5, 0.6))
        local diff = clamp(dot(hit.normal, ld), 0.0, 1.0)
        local amb  = 0.15
        local col  = hit.colour * (diff + amb)
        col = pow(clamp(col, 0.0, 1.0), vec3(0.4545))
        return vec4(col, 1.0)
    end
end)

return myShader
