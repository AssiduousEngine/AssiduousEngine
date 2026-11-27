# Luau.Engine (Plugins::Luau.Engine) - the part every host loads: it registers the Luau backend, so
# a run can start a Luau context, and takes it back as the plugin unloads.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PUBLIC Foundation::Core Foundation::Runtime Foundation::Script Foundation::Policy)
target_sources(${PLUGIN_PART} PRIVATE LuauEngine.cpp) # the part's entry
