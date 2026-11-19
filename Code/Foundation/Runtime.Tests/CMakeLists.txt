# Foundation::Runtime unit tests (doctest).

# A development package's part as a shared library, loaded by the plugin host's shared-part test.
add_library(Runtime.TestPluginPart MODULE TestPluginPart.cpp)
set_target_properties(Runtime.TestPluginPart PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(Runtime.TestPluginPart PRIVATE Foundation::Runtime Foundation::Policy)
util_plugin_abi(Runtime.TestPluginPart)
# The same part built for another engine: its stamp is not this one's, so it is refused.
add_library(Runtime.TestPluginPartOtherAbi MODULE TestPluginPart.cpp)
set_target_properties(Runtime.TestPluginPartOtherAbi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(Runtime.TestPluginPartOtherAbi PRIVATE Foundation::Runtime Foundation::Policy)
target_compile_definitions(Runtime.TestPluginPartOtherAbi PRIVATE "PLUGIN_ABI_STAMP=u8\"another-engine\"")
# The same part as an engine built before parts carried a stamp: it exports no stamp at all.
add_library(Runtime.TestPluginPartNoAbi MODULE TestPluginPart.cpp)
set_target_properties(Runtime.TestPluginPartNoAbi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(Runtime.TestPluginPartNoAbi PRIVATE Foundation::Runtime Foundation::Policy)
target_compile_definitions(Runtime.TestPluginPartNoAbi PRIVATE TEST_PLUGIN_PART_NO_ABI)

add_executable(Runtime.Tests
    TestsMain.cpp
    RuntimeTests.cpp
    PluginManifestTests.cpp
    PluginHostTests.cpp
    PluginRegistryTests.cpp)

target_link_libraries(Runtime.Tests PRIVATE Foundation::Runtime Foundation::Runtime.Client
    Foundation::Scene Foundation::Scene.Resource # the cross-boundary scene-contribution case
    doctest::doctest)
add_dependencies(Runtime.Tests Runtime.TestPluginPart Runtime.TestPluginPartOtherAbi Runtime.TestPluginPartNoAbi)

target_compile_definitions(Runtime.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    TEST_PLUGIN_PART_PATH="$<IF:$<BOOL:${ANDROID}>,$<TARGET_FILE_NAME:Runtime.TestPluginPart>,$<TARGET_FILE:Runtime.TestPluginPart>>"
    TEST_PLUGIN_PART_OTHER_ABI_PATH="$<IF:$<BOOL:${ANDROID}>,$<TARGET_FILE_NAME:Runtime.TestPluginPartOtherAbi>,$<TARGET_FILE:Runtime.TestPluginPartOtherAbi>>"
    TEST_ENGINE_LINKAGE="${UTIL_PLUGIN_ABI_LINKAGE}"
    TEST_PLUGIN_PART_NO_ABI_PATH="$<IF:$<BOOL:${ANDROID}>,$<TARGET_FILE_NAME:Runtime.TestPluginPartNoAbi>,$<TARGET_FILE:Runtime.TestPluginPartNoAbi>>"
    PLUGIN_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Fixtures"
    PLUGIN_MANIFESTS_READ="${UTIL_PLUGIN_MANIFESTS_READ}"
    PLUGIN_ROLES_FILE="${UTIL_PLUGIN_ROLES_FILE}")

# Two fixture packages, probe depending on probe_base, linked the way a cook links its plugins:
# the generated registry lists both (the dependency closure), with the parts a cook loads.
util_add_plugin(Fixtures/ProbeBase)
util_add_plugin(Fixtures/Probe)
util_link_plugins(Runtime.Tests ROLE cook PLUGINS probe)

# The four-part fixture goes through the build's manifest reader too, so the pin in
# PluginManifestTests.cpp always covers dependencies and every part kind.
util_read_plugin_manifest("${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/Everything/Plugin.xml" _fixture)
util_record_plugin_manifest("${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/Everything/Plugin.xml"
    "${_fixture_ID}" "${_fixture_PARTS}" "${_fixture_DEPENDS}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/Everything/Plugin.xml")

target_compile_options(Runtime.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

# The cross-boundary identity plugin part, only in shared-engine builds (the supported
# plugin model). It links the engine's .so set, so identity and rendezvous are
# single-instance by construction; the test proves it across a real dlopen boundary.
if(ENGINE_SHARED_LIBS)
    add_library(Runtime.CrossPlugin MODULE CrossPlugin.cpp)
    set_target_properties(Runtime.CrossPlugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(Runtime.CrossPlugin PRIVATE Foundation::Runtime Foundation::Scene
        Foundation::Policy) # contributes a component manager
    util_plugin_abi(Runtime.CrossPlugin)
    add_dependencies(Runtime.Tests Runtime.CrossPlugin)
    target_compile_definitions(Runtime.Tests PRIVATE
        CROSS_PLUGIN_PATH="$<TARGET_FILE:Runtime.CrossPlugin>")
endif()

add_test(NAME Runtime.Tests COMMAND Runtime.Tests)
