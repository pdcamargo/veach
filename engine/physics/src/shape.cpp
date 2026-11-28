// Shapes (physics.hpp): thin wrappers over Jolt's, shared by reference count.

#include "jolt.hpp"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <format>
#include <memory>

namespace veach::physics {
namespace {

Shape wrap(JPH::RefConst<JPH::Shape> shape) {
    auto impl = std::make_shared<Shape::Impl>();
    impl->shape = std::move(shape);
    return Shape(std::move(impl));
}

Result<Shape> wrap(const JPH::ShapeSettings::ShapeResult& result, std::string_view what) {
    if (result.HasError()) {
        return make_error(ErrorCode::physics_shape_invalid, std::format("{}: {}", what, result.GetError().c_str()),
                          "Check the shape's dimensions and, for meshes, that every index names a vertex and no "
                          "triangle is degenerate.");
    }
    return wrap(result.Get());
}

}  // namespace

Shape Shape::box(const math::vec3& half) {
    initialize();
    return wrap(new JPH::BoxShape(detail::fvec(half)));
}

Shape Shape::sphere(float radius) {
    initialize();
    return wrap(new JPH::SphereShape(radius));
}

Shape Shape::capsule(float half_height, float radius) {
    initialize();
    return wrap(new JPH::CapsuleShape(half_height, radius));
}

Result<Shape> Shape::offset(const Shape& inner, const math::vec3& translation, const math::quat& rotation) {
    initialize();
    if (!inner) {
        return make_error(ErrorCode::physics_shape_invalid, "offset of an empty shape", "Pass a shape to move.");
    }
    const JPH::RotatedTranslatedShapeSettings settings(
        detail::fvec(translation), JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w), inner.impl()->shape);
    return wrap(settings.Create(), "offset shape");
}

Result<Shape> Shape::mesh(std::span<const math::vec3> vertices,
                          std::span<const std::array<std::uint32_t, 3>> triangles) {
    initialize();
    JPH::VertexList v;
    v.reserve(vertices.size());
    for (const math::vec3& p : vertices) {
        v.push_back(JPH::Float3(p.x, p.y, p.z));
    }
    JPH::IndexedTriangleList t;
    t.reserve(triangles.size());
    for (const auto& tri : triangles) {
        t.push_back(JPH::IndexedTriangle(tri[0], tri[1], tri[2], 0));
    }
    const JPH::MeshShapeSettings settings(std::move(v), std::move(t));
    return wrap(settings.Create(), "mesh shape");
}

Result<Shape> Shape::height_field(std::span<const float> heights, std::uint32_t samples, const math::vec3& offset,
                                  const math::vec3& scale) {
    initialize();
    if (heights.size() != static_cast<std::size_t>(samples) * samples) {
        return make_error(ErrorCode::physics_shape_invalid,
                          std::format("height field: {} heights for {} x {} samples", heights.size(), samples, samples),
                          "Pass samples * samples heights, row by row.");
    }
    const JPH::HeightFieldShapeSettings settings(heights.data(), detail::fvec(offset), detail::fvec(scale), samples);
    return wrap(settings.Create(), "height field");
}

std::uint64_t Shape::hash() const {
    if (!impl_) {
        return 0;
    }
    detail::HashStream stream;
    impl_->shape->SaveBinaryState(stream);
    return stream.value;
}

std::uint32_t Shape::triangle_count() const {
    return impl_ ? impl_->shape->GetStats().mNumTriangles : 0;
}

}  // namespace veach::physics
