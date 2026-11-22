// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Script.Pipeline - the script domain's New Asset creators: one per tier (Behavior, Level, Game)
// for every registered script backend that has a cook, each seeded from the cook's tier starter
// (not hard-coded text) written to the sources folder.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module script.pipeline;

import foundation.core;
import foundation.content;
import foundation.script;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    foundation::content::Instance* CreateScriptInstance(const AssetCreationContext& context,
                                                        StringView languageId,
                                                        StringView extension, foundation::script::BehaviorKind kind,
                                                        StringView baseName)
    {
        IScriptLanguageCook* cook = ScriptLanguageCookRegistry::Get().FindByLanguage(languageId);
        if (cook == nullptr)
        {
            return nullptr;
        }
        foundation::content::Group* target = context.Target();
        if (target == nullptr)
        {
            return nullptr;
        }
        // The name first: a script's class is named after the asset that holds it, whatever its kind.
        const String name = target->UniqueInstanceName(context.NameOr(baseName));
        AssetCreationContext named = context;
        named.name = name.AsView();
        String dottedExtension(u8".");
        dottedExtension.Append(extension);
        ScriptClassAsset asset;
        asset.language = String(languageId);
        const String className = ClassNameFor(name.AsView(), baseName);
        const String starter = cook->NewAssetTemplate(kind, className.AsView());
        return CreateLinkedTextAsset(named, baseName, dottedExtension.AsView(), starter.AsView(),
                                     ScriptClassAsset::StaticType(), asset, foundation::vfs::kSourcesCodeFolder);
    }

    foundation::content::Instance* CreateScriptProjectInstance(const AssetCreationContext& context, StringView languageId,
                                                               StringView dottedExtension)
    {
        IScriptAssemblyCook* cook = ScriptAssemblyCookRegistry::Get().FindByLanguage(languageId);
        foundation::content::Group* target = context.Target();
        if (cook == nullptr || target == nullptr)
        {
            return nullptr;
        }
        const String name = target->UniqueInstanceName(context.NameOr(u8"Game"));
        AssetCreationContext named = context;
        named.name = name.AsView();
        ScriptAssemblyAsset asset;
        asset.language = String(languageId);
        const String project = cook->NewProjectTemplate(ClassNameFor(name.AsView(), u8"Game").AsView());
        foundation::content::Instance* created =
            CreateLinkedTextAsset(named, u8"Game", dottedExtension, project.AsView(), ScriptAssemblyAsset::StaticType(), asset,
                                  foundation::vfs::kSourcesCodeFolder);
        // The project file lands in the code folder; its local files go beside it, so the project
        // opens in an IDE at once rather than after its first cook.
        if (created != nullptr)
        {
            RefPtr<ISerializable> made = created->ReadObject(); // beside the project file, in its folder
            const auto* madeAsset = Cast<Asset>(made.Get());
            const String folder(madeAsset != nullptr ? PathParent(madeAsset->fileName.View()) : StringView{});
            for (const ScriptAssemblyFile& file : cook->LocalFiles())
            {
                const String inFolder = folder.IsEmpty() ? String(file.path) : PathJoin(folder.AsView(), file.path.AsView());
                const String path = context.FileOf(inFolder.AsView());
                (void)CreateDirectories(PathParent(path.AsView()));
                (void)WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(file.text.Data()), file.text.Size()));
            }
        }
        return created;
    }

    const TypeInfo* NativeBehaviorClassNamed(StringView name)
    {
        const TypeInfo* found = nullptr;
        for (const TypeInfo* type : GlobalTypeRegistry().All())
        {
            if (StringView(reinterpret_cast<const utf8char*>(type->name)) != name ||
                NativeBehaviorKindOf(*type) == foundation::script::BehaviorKind::None)
            {
                continue;
            }
            if (found != nullptr)
            {
                return nullptr; // two of that name, in two namespaces: the asset's field says which
            }
            found = type;
        }
        return found;
    }

    void RegisterNativeBehaviorCreator(AssetCreatorRegistry& registry)
    {
        AssetCreator creator;
        creator.label = String(kNativeBehaviorCreator);
        creator.category = String(u8"Scripts");
        creator.type = &NativeBehaviorAsset::StaticType();
        creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
        {
            foundation::content::Group* target = context.Target();
            if (target == nullptr)
            {
                return nullptr;
            }
            const String name = target->UniqueInstanceName(context.NameOr(u8"NativeBehavior"));
            NativeBehaviorAsset asset;
            if (const TypeInfo* type = NativeBehaviorClassNamed(name.AsView()))
            {
                asset.className = Format(u8"{}::{}", StringView(reinterpret_cast<const utf8char*>(type->namespaceName)),
                                         StringView(reinterpret_cast<const utf8char*>(type->name)));
            }
            return CreateWrittenInstance(target, name.AsView(), NativeBehaviorAsset::StaticType(), asset);
        };
        registry.Register(Move(creator));
    }

    usize RegisterScriptCreators(AssetCreatorRegistry& registry)
    {
        using foundation::script::BehaviorKind;
        // A creator per kind, named for it ("Luau Entity Behavior").
        static const BehaviorKind kKinds[] = {BehaviorKind::Entity, BehaviorKind::Scene, BehaviorKind::Run};
        usize registered = 0;
        for (const foundation::script::ScriptBackendDesc& backend :
             foundation::script::ScriptBackendRegistry::Get().All())
        {
            // A backend with no cook (compile and harvest) cannot seed a starter.
            if (ScriptLanguageCookRegistry::Get().FindByLanguage(backend.languageId.AsView()) ==
                nullptr)
            {
                continue;
            }
            const String extension = backend.fileExtensions.IsEmpty()
                                         ? String(backend.languageId.AsView())
                                         : String(backend.fileExtensions[0].AsView());
            const StringView displayName = backend.displayName.IsEmpty()
                                               ? backend.languageId.AsView()
                                               : backend.displayName.AsView();
            for (const BehaviorKind kind : kKinds)
            {
                AssetCreator creator;
                creator.label = Format(u8"{} {}", displayName, foundation::script::BehaviorKinds::TypeName(kind));
                creator.category = String(u8"Scripts");
                creator.type = &ScriptClassAsset::StaticType();
                creator.run = [languageId = String(backend.languageId.AsView()), extension,
                               kind, base = String(BehaviorBaseName(kind))](
                                  const AssetCreationContext& context)
                {
                    return CreateScriptInstance(context, languageId.AsView(), extension.AsView(),
                                                kind, base.AsView());
                };
                registry.Register(Move(creator));
                ++registered;
            }
            // A language whose classes build together (C#) needs its project file: one creator
            // for it, seeded from the project build's template.
            if (IScriptAssemblyCook* project = ScriptAssemblyCookRegistry::Get().FindByLanguage(backend.languageId.AsView()))
            {
                AssetCreator creator;
                creator.label = Format(u8"{} Project", displayName);
                creator.category = String(u8"Scripts");
                creator.type = &ScriptAssemblyAsset::StaticType();
                creator.run = [languageId = String(backend.languageId.AsView()),
                               projectExtension = Format(u8".{}", project->ProjectExtension())](const AssetCreationContext& context)
                {
                    return CreateScriptProjectInstance(context, languageId.AsView(), projectExtension.AsView());
                };
                registry.Register(Move(creator));
                ++registered;
            }
        }
        return registered;
    }
}
