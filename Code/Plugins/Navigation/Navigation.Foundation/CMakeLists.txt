# Navigation.Foundation (Plugins::Navigation.Foundation) - the navigation plugin's types: the navmesh
# built and queried over Recast and Detour (`foundation.navigation`) and the cooked navigation zone
# (`foundation.navigation.resource`). Not a part: every part links it, and no host loads it.
util_add_plugin_library()
target_link_libraries(${PLUGIN_LIBRARY}
    PUBLIC Foundation::Core Foundation::Resource Foundation::Content Foundation::Policy
    PRIVATE ThirdParty::Recast Foundation::Profiler)

target_sources(${PLUGIN_LIBRARY}
    PRIVATE
        NavigationImpl.cpp   # module implementation unit: all Recast/Detour contact
    PUBLIC
        FILE_SET navigation_foundation_modules TYPE CXX_MODULES FILES
            NavigationModule.cppm
            NavigationBuilder.cppm
            NavigationMesh.cppm
            NavigationResource.cppm)
