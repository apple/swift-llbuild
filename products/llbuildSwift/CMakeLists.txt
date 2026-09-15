
# Set x86_64 default if CMAKE_OSX_ARCHITECTURES is not set
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "")
  set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

# TODO(compnerd) move both of these outside of the CMake into the invocation
if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
  add_compile_options(-target ${CMAKE_OSX_ARCHITECTURES}-apple-macosx10.10)
  if(NOT CMAKE_OSX_SYSROOT STREQUAL "")
    add_compile_options(-sdk ${CMAKE_OSX_SYSROOT})
  endif()
endif()

add_library(llbuildSwift
  BuildSystemBindings.swift
  CoreBindings.swift
  BuildDBBindings.swift
  BuildKey.swift
  Internals.swift
  BuildValue.swift)
if(CMAKE_SYSTEM_NAME STREQUAL Windows)
  target_compile_options(llbuildSwift PRIVATE
    "SHELL:-Xcc -D_CRT_NONSTDC_NO_DEPRECATE")
endif()
target_link_libraries(llbuildSwift PRIVATE
    llbuild)
if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
  set_target_properties(llbuildSwift PROPERTIES
    # RUNPATH for finding Swift core libraries in the toolchain.
    # FIXME: ideally, the second path should be passed from the swift-ci invocation
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "@loader_path:@loader_path/../../macosx"
    MACOSX_RPATH TRUE)
  if(CMAKE_VERSION VERSION_LESS 3.16)
    target_link_options(llbuildSwift PRIVATE
      "SHELL:-Xlinker -install_name -Xlinker @rpath/$<TARGET_FILE_NAME:llbuildSwift>")
  else()
    set_target_properties(llbuildSwift PROPERTIES
      BUILD_WITH_INSTALL_NAME_DIR TRUE
      INSTALL_NAME_DIR "@rpath")
  endif()
else()
  target_link_libraries(llbuildSwift PRIVATE
    SQLite::SQLite3)
  if(dispatch_FOUND)
    target_link_libraries(llbuildSwift PRIVATE
      swiftDispatch)
  endif()
  if(Foundation_FOUND)
    target_link_libraries(llbuildSwift PRIVATE
      Foundation)
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL Windows)
    target_link_options(llbuildSwift PRIVATE "SHELL:-no-toolchain-stdlib-rpath")
    set_target_properties(llbuildSwift PROPERTIES
      INSTALL_RPATH "$ORIGIN/../../$<LOWER_CASE:${CMAKE_SYSTEM_NAME}>")
  endif()
endif()
set_target_properties(llbuildSwift PROPERTIES
  Swift_MODULE_NAME llbuildSwift
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${PROJECT_SOURCE_DIR}/products/libllbuild/include")

install(TARGETS llbuildSwift
  ARCHIVE DESTINATION lib/swift/pm/llbuild COMPONENT libllbuildSwift
  LIBRARY DESTINATION lib/swift/pm/llbuild COMPONENT libllbuildSwift
  RUNTIME DESTINATION bin COMPONENT libllbuildSwift)
if(MSVC)
  install(FILES $<TARGET_FILE_DIR:llbuildSwift>/$<TARGET_FILE_BASE_NAME:llbuildSwift>.pdb
    DESTINATION bin
    OPTIONAL)
endif()
set_property(GLOBAL APPEND PROPERTY LLBuild_EXPORTS llbuildSwift)

# Add install target.
add_custom_target(install-libllbuildSwift
  DEPENDS llbuildSwift
  COMMENT "Installing libllbuildSwift..."
  COMMAND "${CMAKE_COMMAND}"
    -DCMAKE_INSTALL_COMPONENT=libllbuildSwift
    -P "${CMAKE_BINARY_DIR}/cmake_install.cmake")
