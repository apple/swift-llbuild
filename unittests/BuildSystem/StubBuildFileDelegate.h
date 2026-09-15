//===- unittests/BuildSystem/StubBuildFileDelegate.h ------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2026 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef LLBUILD_TESTS_STUBBUILDFILEDELEGATE_H
#define LLBUILD_TESTS_STUBBUILDFILEDELEGATE_H

#include "llbuild/Basic/FileSystem.h"
#include "llbuild/BuildSystem/BuildDescription.h"
#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/ShellCommand.h"
#include "llbuild/BuildSystem/Tool.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include "TempDir.h"

#include "gtest/gtest.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llbuild {
namespace unittests {

/// Hands back real ShellCommands, so tests exercise the same configuration path
/// a build does. Attributes the tool itself doesn't know about are accepted,
/// since these tests are about commands rather than tool defaults.
class StubShellTool : public buildsystem::Tool {
public:
  using Tool::Tool;

  bool configureAttribute(const buildsystem::ConfigureContext&, StringRef,
                          StringRef) override {
    return true;
  }
  bool configureAttribute(const buildsystem::ConfigureContext&, StringRef,
                          ArrayRef<StringRef>) override {
    return true;
  }
  bool configureAttribute(const buildsystem::ConfigureContext&, StringRef,
                          ArrayRef<std::pair<StringRef, StringRef>>) override {
    return true;
  }

  std::unique_ptr<buildsystem::Command> createCommand(StringRef name) override {
    return llvm::make_unique<buildsystem::ShellCommand>(name,
                                                        /*controlEnabled=*/true);
  }
};

/// The smallest delegate that can drive a build description, whether it is being
/// parsed from a build file or assembled directly through a
/// `BuildDescriptionBuilder`. Recording the diagnostics rather than printing
/// them lets tests assert on them.
///
/// Interned strings are held for the lifetime of the delegate, matching the
/// contract the real loader relies on: an environment base hands out StringRefs
/// that the commands referencing it keep.
class StubBuildFileDelegate : public buildsystem::BuildFileDelegate {
  llvm::StringMap<bool> internedStrings;
  std::unique_ptr<basic::FileSystem> fileSystem = basic::createLocalFileSystem();

public:
  std::vector<std::string> errors;

  StringRef getInternedString(StringRef value) override {
    return internedStrings.insert(std::make_pair(value, true)).first->getKey();
  }
  basic::FileSystem& getFileSystem() override { return *fileSystem; }
  void setFileContentsBeingParsed(StringRef) override {}
  void error(StringRef, const buildsystem::BuildFileToken&,
             const Twine& message) override {
    errors.push_back(message.str());
  }
  void cannotLoadDueToMultipleProducers(
      buildsystem::Node*, std::vector<buildsystem::Command*>) override {}
  bool configureClient(const buildsystem::ConfigureContext&, StringRef, uint32_t,
                       const buildsystem::property_list_type&) override {
    return true;
  }
  std::unique_ptr<buildsystem::Tool> lookupTool(StringRef name) override {
    if (name == "shell")
      return llvm::make_unique<StubShellTool>(name);
    return nullptr;
  }
  void loadedTarget(StringRef, const buildsystem::Target&) override {}
  void loadedDefaultTarget(StringRef) override {}
  void loadedCommand(StringRef, const buildsystem::Command&) override {}
  std::unique_ptr<buildsystem::Node> createNode(StringRef name,
                                                bool isImplicit) override {
    return buildsystem::BuildNode::makePlain(name);
  }
};

/// Load a build file from `contents`, which is written to a temporary file so
/// the real on-disk loader runs.
inline std::unique_ptr<buildsystem::BuildDescription>
loadBuildFile(StubBuildFileDelegate& delegate, const TmpDir& tempDir,
              StringRef contents) {
  llvm::SmallString<256> path(tempDir.str());
  llvm::sys::path::append(path, "build.llbuild");

  std::error_code ec;
  llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::F_Text);
  EXPECT_FALSE(ec);
  os << contents;
  os.close();

  return buildsystem::BuildFile(path, delegate).load();
}

inline const buildsystem::Command*
commandNamed(buildsystem::BuildDescription& description, StringRef name) {
  auto it = description.getCommands().find(name);
  return it == description.getCommands().end() ? nullptr : it->second.get();
}

/// The effective environment of a shell command, flattened to `KEY=VALUE` in
/// order, which is what the environment-ordering assertions are really about.
inline std::vector<std::string>
effectiveEnvOf(const buildsystem::Command* command) {
  llvm::SmallVector<std::pair<StringRef, StringRef>, 8> env;
  static_cast<const buildsystem::ShellCommand*>(command)->getEffectiveEnv(env);

  std::vector<std::string> result;
  for (const auto& entry: env)
    result.push_back((entry.first + "=" + entry.second).str());
  return result;
}

}
}

#endif /* LLBUILD_TESTS_STUBBUILDFILEDELEGATE_H */
