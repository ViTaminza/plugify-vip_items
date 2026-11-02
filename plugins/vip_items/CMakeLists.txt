# vip_items — Plugify C++ plugin: port of the Pisex VIP module "VIP_Items".
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The map gate, the
# client checks and the map-gated schema offsets live in the plugin's own src/guards.hpp; the
# engine-free decisions are shared with the test through src/items_logic.hpp.
#
#   plugify-vip_items  the plugin shared library (entry "bin/vip_items")
#   vip_items_tests    engine-free unit tests: the tokenizer, the round condition, the grenade ammo
#                      slots, the carried-weapon helpers and the KV1 reader, plus the map gate,
#                      IsConnected/IsAlive and the schema cache driven through fake s2sdk bindings
#                      (build/tests/vip_items_tests)

set(VIP_ITEMS_VERSION "1.1.0")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*   (s2sdk_bindings)
#   __vip_*     (vip consumer bindings, generated into plugins/vip/include/external/plugify/include)
# In the monorepo plugins/vip is added first ("vip" < "vip_items"), so vip_bindings already exists;
# a standalone exported repo only carries plugins/vip/include/ headers + vip.cpp and has no
# plugins/vip/CMakeLists.txt - define vip_bindings here when absent.
if(NOT TARGET vip_bindings)
    add_library(vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-vip_items SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(plugify-vip_items PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-vip_items SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-vip_items PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-vip_items PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-vip_items PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_items"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-vip_items PROPERTIES OUTPUT_NAME vip_items)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-vip_items PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/vip_items.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vip_items.pplugin"
)

# --- tests --------------------------------------------------------------------------------------
# vip_items_tests: no engine and no pisexcompat. The pure half (tokenizer, round condition, grenade
# ammo slots, carried-weapon helpers, the KV1 reader on the production-shaped file in tests/data)
# touches no binding; the guard half installs fake __s2sdk_* bindings and checks that no client,
# entity or schema query leaves the plugin outside a live map, that IsConnected/IsAlive apply the
# PITFALLS C2 chain and that SchemaOffset is map-gated and caches only successful lookups (C8).
add_executable(vip_items_tests
        tests/vip_items_tests.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(vip_items_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vip_items_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_items_tests PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(vip_items_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_items_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        VIP_ITEMS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(vip_items_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
