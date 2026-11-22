// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script - :behavior_kind partition. The three kinds of class the engine runs, by the
// scope each lives in: a RunBehavior for the run, a SceneBehavior for a scene, an EntityBehavior
// for an entity. A class declares its kind by deriving from one, in every language; each language
// names the kinds as the base types listed here, and a cook records the kind it found.
module;
#include "Core/Prelude.h"

export module foundation.script:behavior_kind;

import foundation.core;

export namespace foundation::script
{
    enum class BehaviorKind : core::u8
    {
        None,   // not a behavior (a module of helpers, or a class deriving from no kind)
        Run,    // lives for the run, across scene loads; constructed with nothing
        Scene,  // one per scene; constructed with its scene
        Entity, // one per attached slot on an entity; constructed with its entity
    };

    /// The language a native class's BehaviorClass records: a C++ class deriving from one of the
    /// kinds' engine bases, named by its qualified reflected name ("rtti::game::Orb").
    inline constexpr core::StringView kNativeLanguage = u8"native";
    /// The reflection namespace of the engine's native bases, whose names are the kinds' base
    /// types' (RunBehavior, SceneBehavior, EntityBehavior): how a native class's kind is read.
    inline constexpr const char* kNativeBehaviorNamespace = "rtti::behavior";
    /// The field attribute that makes a native class's reflected field one of its properties: what
    /// the inspector shows and an entity or a scene overrides.
    inline constexpr const char* kBehaviorPropertyAttribute = "behaviorProperty";

    // Static members rather than free functions: gcc 15 cannot read back a module partition whose
    // namespace-scope inline function instantiates a template.
    struct BehaviorKinds
    {
        /// Every kind a class can declare, in the order Run, Scene, Entity.
        // ENGINE_EXPORT_DATA: odr-used from another library (the backends loop over it).
        static constexpr ENGINE_EXPORT_DATA BehaviorKind kAll[] = {BehaviorKind::Run, BehaviorKind::Scene,
                                                                  BehaviorKind::Entity};

        /// The base type a class derives from to be of `kind`, as every language names it
        /// ("RunBehavior"); empty for None.
        [[nodiscard]] static constexpr core::StringView TypeName(BehaviorKind kind) noexcept
        {
            switch (kind)
            {
            case BehaviorKind::Run:
                return u8"RunBehavior";
            case BehaviorKind::Scene:
                return u8"SceneBehavior";
            case BehaviorKind::Entity:
                return u8"EntityBehavior";
            case BehaviorKind::None:
            default:
                return core::StringView{};
            }
        }

        /// The kind whose base type is named `typeName`; None when no kind is.
        [[nodiscard]] static constexpr BehaviorKind FromTypeName(core::StringView typeName) noexcept
        {
            for (const BehaviorKind kind : kAll)
            {
                if (TypeName(kind) == typeName)
                {
                    return kind;
                }
            }
            return BehaviorKind::None;
        }
    };
}
