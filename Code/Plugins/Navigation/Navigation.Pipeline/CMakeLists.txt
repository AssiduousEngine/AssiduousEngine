# Navigation.Pipeline (Plugins::Navigation.Pipeline) - `navigation.pipeline`: the navigation zone
# asset, its builder and File > New's creator. The editor and the tools load it.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Pipeline::Core Pipeline::Importer Foundation::Content Foundation::Policy
    PRIVATE Foundation::Runtime)

target_sources(${PLUGIN_PART}
    PRIVATE
        NavigationPipeline.cpp     # the part's entry
        NavigationCreatorsImpl.cpp # File > New, asset_create
    PUBLIC
        FILE_SET navigation_pipeline_modules TYPE CXX_MODULES FILES NavigationAsset.cppm)
