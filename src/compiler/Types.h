#pragma once
#include <string>
#include <vector>
#include <cassert>

// ── GLSL type tags ────────────────────────────────────────────────────────────
enum class GlslType {
    Float, Int, Bool,
    Vec2, Vec3, Vec4,
    IVec2, IVec3, IVec4,
    Mat2, Mat3, Mat4,
    Sampler2D,
    Void,
    Struct,   // anonymous; identity determined by struct_id after dedup
    Unknown,  // unresolved type variable / error sentinel
};

// Returns the GLSL keyword string for a concrete type (not Struct/Unknown).
const char *glsl_type_name(GlslType t);

// Returns true if t is a float-family vector (vec2/3/4)
inline bool is_float_vec(GlslType t) {
    return t == GlslType::Vec2 || t == GlslType::Vec3 || t == GlslType::Vec4;
}
inline bool is_int_vec(GlslType t) {
    return t == GlslType::IVec2 || t == GlslType::IVec3 || t == GlslType::IVec4;
}
inline bool is_vec(GlslType t) { return is_float_vec(t) || is_int_vec(t); }
inline bool is_mat(GlslType t) {
    return t == GlslType::Mat2 || t == GlslType::Mat3 || t == GlslType::Mat4;
}
inline bool is_scalar(GlslType t) {
    return t == GlslType::Float || t == GlslType::Int || t == GlslType::Bool;
}
inline int vec_dim(GlslType t) {
    switch (t) {
        case GlslType::Vec2: case GlslType::IVec2: return 2;
        case GlslType::Vec3: case GlslType::IVec3: return 3;
        case GlslType::Vec4: case GlslType::IVec4: return 4;
        default: return 0;
    }
}
inline int mat_dim(GlslType t) {
    switch (t) {
        case GlslType::Mat2: return 2;
        case GlslType::Mat3: return 3;
        case GlslType::Mat4: return 4;
        default: return 0;
    }
}
// Scalar base of a vector type
inline GlslType vec_scalar(GlslType t) {
    return is_int_vec(t) ? GlslType::Int : GlslType::Float;
}
// Vector type for given scalar and dimension
inline GlslType make_vec(GlslType scalar, int dim) {
    if (scalar == GlslType::Int) {
        if (dim == 2) return GlslType::IVec2;
        if (dim == 3) return GlslType::IVec3;
        return GlslType::IVec4;
    }
    if (dim == 2) return GlslType::Vec2;
    if (dim == 3) return GlslType::Vec3;
    return GlslType::Vec4;
}

// ── TypeInfo ──────────────────────────────────────────────────────────────────
struct TypeInfo {
    GlslType tag       = GlslType::Unknown;
    int      struct_id = -1;  // valid when tag == Struct
    int      tvar_id   = -1;  // index into union-find table; -1 if concrete

    static TypeInfo make(GlslType t) {
        TypeInfo ti; ti.tag = t; return ti;
    }
    static TypeInfo make_tvar(int id) {
        TypeInfo ti; ti.tag = GlslType::Unknown; ti.tvar_id = id; return ti;
    }
    static TypeInfo make_struct(int sid) {
        TypeInfo ti; ti.tag = GlslType::Struct; ti.struct_id = sid; return ti;
    }
    static TypeInfo unknown() { return TypeInfo{}; }

    bool is_concrete() const { return tvar_id == -1 && tag != GlslType::Unknown; }
    bool is_tvar()     const { return tvar_id != -1; }
    bool is_unknown()  const { return tag == GlslType::Unknown && tvar_id == -1; }

    bool operator==(const TypeInfo &o) const {
        if (tag != o.tag) return false;
        if (tag == GlslType::Struct) return struct_id == o.struct_id;
        return true;
    }
    bool operator!=(const TypeInfo &o) const { return !(*this == o); }
};

// ── Union-find for type variables ─────────────────────────────────────────────
struct TNode {
    int      parent    = -1;     // -1 = root
    TypeInfo concrete  = {};     // valid when resolved
    bool     is_resolved = false;
};

class UnionFind {
public:
    // Allocate a new type variable; return its index.
    int new_tvar();

    // Find the representative of i (path-compressed).
    int find(int i);

    // Unify two type variables (or a tvar with a concrete type).
    // Returns false and sets conflict if they are already resolved to
    // different concrete types.
    bool unify(int a, int b, TypeInfo &conflict_a, TypeInfo &conflict_b);

    // Constrain a type variable to a concrete type.
    // Returns false on conflict.
    bool constrain(int var, TypeInfo ty, TypeInfo &existing);

    // Resolve a TypeInfo: if it's a tvar, follow the union-find to get concrete.
    TypeInfo resolve(TypeInfo ti);

    const std::vector<TNode> &nodes() const { return nodes_; }

private:
    std::vector<TNode> nodes_;
};
