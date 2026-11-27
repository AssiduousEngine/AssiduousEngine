# Navigation.Editor (Plugins::Navigation.Editor) - `editor.navigation`: the bake, its preferences,
# the zone's gizmo, the inspector's Bake Navigation action and the navigation_bake MCP tool.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PUBLIC
    Foundation::Core Foundation::Scene Foundation::Content Foundation::Json Foundation::Mcp
    Editor::Core Editor::Scene Editor::Mcp Foundation::Settings Foundation::Render Foundation::Policy)
target_sources(${PLUGIN_PART}
    PRIVATE
        NavigationEditor.cpp       # the part's entry
        NavigationBakeImpl.cpp     # bake logic and heavy imports (kept out of the interface unit)
        NavigationEditorTools.cpp  # the gizmo, the inspector action, the MCP tool
    PUBLIC
        FILE_SET editor_navigation_modules TYPE CXX_MODULES FILES NavigationBake.cppm)
