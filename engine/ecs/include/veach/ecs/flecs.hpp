#pragma once

// The Flecs barrel (ADR 0006). This is an alias layer, NOT a wrapper: engine and
// game C++ code use Flecs v4 idioms directly through these names. Outside
// engine/, never write `flecs::` — use the veach:: aliases, or veach::ecs:: for
// anything that has no alias yet.

#include <flecs.h>

namespace veach {

/// Raw Flecs, for advanced cases that have no veach:: alias.
namespace ecs = ::flecs;

using World = flecs::world;
using Entity = flecs::entity;
using EntityView = flecs::entity_view;
using Id = flecs::id;
using Iter = flecs::iter;
using System = flecs::system;
using Observer = flecs::observer;
using Cursor = flecs::cursor;

template <class... Components>
using Query = flecs::query<Components...>;
template <class... Components>
using QueryBuilder = flecs::query_builder<Components...>;
template <class T>
using Component = flecs::component<T>;

// Builtin relationships, traits and events.
using flecs::ChildOf;
using flecs::IsA;
using flecs::OnAdd;
using flecs::OnRemove;
using flecs::OnSet;
using flecs::Singleton;
using flecs::Wildcard;

}  // namespace veach
