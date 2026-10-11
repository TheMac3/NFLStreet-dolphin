enable_testing()
add_custom_target(unittests)
add_custom_command(TARGET unittests POST_BUILD COMMAND ${CMAKE_CTEST_COMMAND} "--output-on-failure")

string(APPEND CMAKE_RUNTIME_OUTPUT_DIRECTORY "/Tests")

add_executable(tests EXCLUDE_FROM_ALL UnitTestsMain.cpp StubHost.cpp)
set_target_properties(tests PROPERTIES FOLDER Tests)
target_link_libraries(tests PRIVATE fmt::fmt gtest::gtest core uicommon)
add_test(NAME tests COMMAND tests)
add_custom_command(TARGET tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Sys"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Data/Sys" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Sys"
)
add_dependencies(unittests tests)

# The rollback protocol and pure rollback decisions deliberately build without
# DolphinLib, ENet, or sockets. This keeps the regression gate quick enough for
# every Linux and Windows build while the larger UnitTests target remains opt-in.
add_executable(nsr_rollback_tests EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/Tools/NSR/NSRRollbackNetplayTest.cpp
  ${CMAKE_SOURCE_DIR}/Source/Core/Core/NSR/NSRRollbackNetplay.cpp
)
target_include_directories(nsr_rollback_tests PRIVATE ${CMAKE_SOURCE_DIR}/Source/Core)
if(MSVC)
  target_compile_options(nsr_rollback_tests PRIVATE /W4 /WX /UNDEBUG)
else()
  target_compile_options(nsr_rollback_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
endif()
set_target_properties(nsr_rollback_tests PROPERTIES FOLDER Tests)
add_test(NAME nsr_rollback_tests COMMAND nsr_rollback_tests)
add_dependencies(unittests nsr_rollback_tests)

macro(add_dolphin_test target)
  add_library(${target} OBJECT ${ARGN})
  target_link_libraries(${target} PUBLIC fmt::fmt gtest::gtest PRIVATE core uicommon)
  target_link_libraries(tests PRIVATE ${target})
endmacro()

add_subdirectory(Common)
add_subdirectory(Core)
add_subdirectory(VideoCommon)
