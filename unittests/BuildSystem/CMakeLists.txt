add_llbuild_unittest(BuildSystemTests
  BuildDescriptionBuilderTest.cpp
  BuildDescriptionLoadPerfTest.cpp
  BuildSystemFrontendTest.cpp
  BuildSystemTaskTests.cpp
  BuildValueTest.cpp
  ShellCommandTest.cpp
  MockBuildSystemDelegate.cpp
  TempDir.cpp
  )

target_link_libraries(BuildSystemTests PRIVATE
  llbuildBuildSystem
  llvmSupport
  llbuildCore
  llbuildBasic
  llvmSupport
  SQLite::SQLite3)

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
  target_link_libraries(BuildSystemTests PRIVATE
    curses)
endif()
