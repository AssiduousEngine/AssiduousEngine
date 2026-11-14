// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Script - the `editor.script` module.
//
// ScriptApiSurface implementation: the single bound-API build for a page. It replays the
// runtime's registration sequence (as ScriptSubsystem runs it) against a throwaway manager. The
// registries are idempotent and the manager is destroyed right after DescribeBoundApi; the result
// is cached for the page's lifetime. Completion and the API browser both read it.

module;
#include "Core/Prelude.h"

module editor.script;

import foundation.core;
import foundation.script;
import foundation.script.facades;
import script.pipeline;

using namespace foundation::core;
namespace core = foundation::core;

namespace editor
{
    namespace script = foundation::script;

    const Array<script::ScriptApiType>& ScriptApiSurface::Types() const
    {
        if (m_built || m_language.IsEmpty())
        {
            return m_types;
        }
        m_built = true; // one attempt; a language without a cook stays empty

        // The language's cook describes the API its check compiles against, from the engine it
        // keeps for checking: building one here as well cost as much again as a check.
        if (pipeline::IScriptLanguageCook* cook = pipeline::ScriptLanguageCookRegistry::Get().FindByLanguage(m_language.AsView()))
        {
            m_types = cook->DescribeApi();
        }
        return m_types;
    }
}
