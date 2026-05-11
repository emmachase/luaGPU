#include "Types.h"
#include <stdexcept>

const char *glsl_type_name(GlslType t) {
    switch (t) {
        case GlslType::Float:     return "float";
        case GlslType::Int:       return "int";
        case GlslType::Bool:      return "bool";
        case GlslType::Vec2:      return "vec2";
        case GlslType::Vec3:      return "vec3";
        case GlslType::Vec4:      return "vec4";
        case GlslType::IVec2:     return "ivec2";
        case GlslType::IVec3:     return "ivec3";
        case GlslType::IVec4:     return "ivec4";
        case GlslType::Mat2:      return "mat2";
        case GlslType::Mat3:      return "mat3";
        case GlslType::Mat4:      return "mat4";
        case GlslType::Sampler2D: return "sampler2D";
        case GlslType::Void:      return "void";
        default: return "<?>";
    }
}

// ── UnionFind ──────────────────────────────────────────────────────────────────

int UnionFind::new_tvar() {
    int id = (int)nodes_.size();
    nodes_.push_back(TNode{});
    return id;
}

int UnionFind::find(int i) {
    while (nodes_[i].parent != -1) {
        // path compression
        if (nodes_[nodes_[i].parent].parent != -1)
            nodes_[i].parent = nodes_[nodes_[i].parent].parent;
        i = nodes_[i].parent;
    }
    return i;
}

bool UnionFind::constrain(int var, TypeInfo ty, TypeInfo &existing) {
    int root = find(var);
    TNode &n = nodes_[root];
    if (n.is_resolved) {
        if (n.concrete == ty) return true;
        // Allow a Float-defaulted numeric literal tvar to be tightened to Int
        // when context (e.g. `int(3) + 1`) demands it.
        if (n.concrete.tag == GlslType::Float && ty.tag == GlslType::Int) {
            n.concrete = ty;
            return true;
        }
        existing = n.concrete;
        return false;
    }
    n.concrete    = ty;
    n.is_resolved = true;
    return true;
}

bool UnionFind::unify(int a, int b, TypeInfo &conflict_a, TypeInfo &conflict_b) {
    int ra = find(a);
    int rb = find(b);
    if (ra == rb) return true;

    TNode &na = nodes_[ra];
    TNode &nb = nodes_[rb];

    if (na.is_resolved && nb.is_resolved) {
        if (na.concrete != nb.concrete) {
            conflict_a = na.concrete;
            conflict_b = nb.concrete;
            return false;
        }
        // merge rb into ra
        nb.parent = ra;
        return true;
    }
    if (na.is_resolved) {
        nb.parent = ra;
        return true;
    }
    if (nb.is_resolved) {
        na.parent = rb;
        return true;
    }
    // neither resolved — merge smaller into larger (by index)
    if (ra < rb) nb.parent = ra;
    else         na.parent = rb;
    return true;
}

TypeInfo UnionFind::resolve(TypeInfo ti) {
    if (!ti.is_tvar()) return ti;
    int root = find(ti.tvar_id);
    if (nodes_[root].is_resolved) return nodes_[root].concrete;
    // unresolved → default float
    return TypeInfo::make(GlslType::Float);
}
