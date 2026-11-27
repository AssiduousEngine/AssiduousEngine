# Terrain.Editor (Plugins::Terrain.Editor) - the terrain plugin's editor: the terrain and vegetation
# mask pages, the sculpt, paint, hole, vegetation paint and scatter tools with their panels, and the
# splatmap thumbnails (`editor.terrain`, `editor.vegetation`).
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PUBLIC
    Foundation::Core Foundation::Content
    Foundation::Runtime Foundation::Runtime.Client Foundation::Graphics
    Foundation::Scene Engine::Scene Foundation::Render Engine::Render
    Foundation::GUI Foundation::GUI.Toolkit
    Editor::Core Editor::App Editor::ViewportTools Editor::Preview
    Pipeline::Core Pipeline::Heightfield
    Foundation::Heightfield Foundation::Heightfield.Resource Foundation::Resource
    Foundation::Physics Engine::Physics # the scatter tool's overlap test
    Foundation::Image Foundation::Image.IO Foundation::Texture.Resource Foundation::Policy)
target_sources(${PLUGIN_PART}
    PRIVATE TerrainEditor.cpp # the part's entry
            AssetEditsImpl.cpp SculptImpl.cpp SplatToolImpl.cpp HoleToolImpl.cpp ToolPanelsImpl.cpp TerrainEditorPageImpl.cpp
            VegetationPickImpl.cpp VegetationPaintToolImpl.cpp VegetationScatterToolImpl.cpp
            VegetationToolPanelsImpl.cpp VegetationMaskPageImpl.cpp
    PUBLIC FILE_SET terrain_editor_modules TYPE CXX_MODULES FILES
        AssetEdits.cppm Sculpt.cppm SplatTool.cppm HoleTool.cppm SplatmapThumbnailGenerator.cppm ToolPanels.cppm
        TerrainEditorPage.cppm EditorTerrainModule.cppm
        VegetationPick.cppm VegetationPaintTool.cppm VegetationScatterTool.cppm
        VegetationMaskPage.cppm VegetationEditorModule.cppm)
