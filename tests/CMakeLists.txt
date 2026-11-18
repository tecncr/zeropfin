set(ZF_UNIT_TESTS
  test_util
  test_config
  test_urlutil
  test_netfilter
  test_auth
  test_zp_proto
  test_player_state
  test_compat
  test_jellyfin_map
  test_relay_range
  test_http_static
)

foreach(t ${ZF_UNIT_TESTS})
  add_executable(${t} unit/${t}.c)
  target_link_libraries(${t} PRIVATE zfcore)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME unit.${t} COMMAND ${t} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# Mock player speaking the ZeroPlay --control protocol (CI never needs a Pi).
add_executable(mock_zeroplay mock_zeroplay/mock_zeroplay.c)
target_link_libraries(mock_zeroplay PRIVATE PkgConfig::CURL)

# Player supervisor tests drive the real zf_player against the mock binary.
add_executable(test_player unit/test_player.c)
target_link_libraries(test_player PRIVATE zfcore)
target_include_directories(test_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_player PRIVATE MOCK_ZEROPLAY="$<TARGET_FILE:mock_zeroplay>")
add_test(NAME component.player COMMAND test_player)
set_tests_properties(component.player PROPERTIES TIMEOUT 120)

# End-to-end HTTP tests: real daemon + mock ZeroPlay + fake Jellyfin (Python stdlib only).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME integration.daemon
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/integration/run.py
            --zeropfin $<TARGET_FILE:zeropfin>
            --mock-zeroplay $<TARGET_FILE:mock_zeroplay>
            --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
            --web-root ${CMAKE_SOURCE_DIR}/web/dist)
  set_tests_properties(integration.daemon PROPERTIES TIMEOUT 300)
endif()
