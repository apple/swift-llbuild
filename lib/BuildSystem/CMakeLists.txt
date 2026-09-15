add_llbuild_library(llbuildBuildSystem STATIC
  BuildDescription.cpp
  BuildDescriptionBuilder.cpp
  BuildFile.cpp
  BuildKey.cpp
  BuildNode.cpp
  BuildSystem.cpp
  BuildSystemExtensionManager.cpp
  BuildSystemFrontend.cpp
  BuildValue.cpp
  ExternalCommand.cpp
  ShellCommand.cpp
  )

target_link_libraries(llbuildBuildSystem PRIVATE
  llbuildCore
  llbuildBasic
  llvmSupport)

set_property(GLOBAL APPEND PROPERTY LLBuild_EXPORTS llbuildBuildSystem)
