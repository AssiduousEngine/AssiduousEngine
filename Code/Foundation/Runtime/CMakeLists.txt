# Foundation::Runtime - the `foundation.runtime` module (SubsystemManager and Subsystems, the
# plugin contract and host, and the plugin manifest, which is XML: hence Xml.Serialization).

util_add_engine_library(Runtime ALIAS Foundation::Runtime)
target_link_libraries(Runtime PUBLIC Foundation::Core Foundation::Run Foundation::Xml.Serialization
    Foundation::Policy)

target_sources(Runtime
    PUBLIC
        FILE_SET runtime_modules TYPE CXX_MODULES FILES
            RuntimeModule.cppm
            Subsystem.cppm
            SubsystemManager.cppm
            PluginLoader.cppm
            PluginManifest.cppm
            EnabledPlugin.cppm
            PluginPart.cppm
            PluginHost.cppm)

# The plugin ABI stamp (cmake/Plugins.cmake), in the one file that answers it.
target_sources(Runtime PRIVATE PluginAbiImpl.cpp)
set_source_files_properties(PluginAbiImpl.cpp PROPERTIES COMPILE_DEFINITIONS "${UTIL_PLUGIN_ABI_DEFINITION}")
