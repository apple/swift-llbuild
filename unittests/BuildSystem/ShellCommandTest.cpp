//===- unittests/BuildSystem/ShellCommandTest.cpp -------------------------===//
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

#include "llbuild/BuildSystem/ShellCommand.h"

#include "llbuild/BuildSystem/BuildDescription.h"

#include "StubBuildFileDelegate.h"
#include "TempDir.h"

#include "gtest/gtest.h"

#include <string>
#include <vector>

using namespace llbuild;
using namespace llbuild::buildsystem;
using namespace llbuild::unittests;
using namespace llvm;

namespace {

// These tests drive the build-file front-end; BuildDescriptionBuilderTest covers
// the same environment behavior reached through the in-memory builder instead.

/// A command with no base keeps its own bindings, unchanged.
TEST(ShellCommandTest, withoutBase) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);
  auto description = loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
commands:
  C:
    tool: shell
    env:
      B: two
      A: one
)");

  ASSERT_TRUE(description != nullptr);
  auto* command = commandNamed(*description, "C");
  ASSERT_TRUE(command != nullptr);
  EXPECT_EQ(effectiveEnvOf(command), std::vector<std::string>({"B=two", "A=one"}));
}

/// A command that names a base but adds nothing gets exactly the base.
TEST(ShellCommandTest, baseWithoutOverrides) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);
  auto description = loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
env-bases:
  common:
    A: one
    B: two
commands:
  C:
    tool: shell
    env-base: common
)");

  ASSERT_TRUE(description != nullptr);
  EXPECT_EQ(description->getEnvironmentBases().size(), 1u);
  EXPECT_EQ(effectiveEnvOf(commandNamed(*description, "C")),
            std::vector<std::string>({"A=one", "B=two"}));
}

/// An override replaces the base's value *in place*, so the effective
/// environment keeps the base's ordering. Signature stability depends on this:
/// the order must not shift just because a key was overridden.
TEST(ShellCommandTest, overrideSubstitutesInPlace) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);
  auto description = loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
env-bases:
  common:
    A: one
    B: two
    C: three
commands:
  Cmd:
    tool: shell
    env-base: common
    env:
      B: overridden
)");

  ASSERT_TRUE(description != nullptr);
  EXPECT_EQ(effectiveEnvOf(commandNamed(*description, "Cmd")),
            std::vector<std::string>({"A=one", "B=overridden", "C=three"}));
}

/// Keys the base does not mention are appended after it.
TEST(ShellCommandTest, overrideAddsNewKey) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);
  auto description = loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
env-bases:
  common:
    A: one
commands:
  Cmd:
    tool: shell
    env-base: common
    env:
      EXTRA: added
)");

  ASSERT_TRUE(description != nullptr);
  EXPECT_EQ(effectiveEnvOf(commandNamed(*description, "Cmd")),
            std::vector<std::string>({"A=one", "EXTRA=added"}));
}

/// Two commands sharing a base must not see each other's bindings.
TEST(ShellCommandTest, siblingsDoNotLeak) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);
  auto description = loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
env-bases:
  common:
    A: one
    B: base
commands:
  First:
    tool: shell
    env-base: common
    env:
      B: first
  Second:
    tool: shell
    env-base: common
    env:
      B: second
      ONLY_SECOND: yes
)");

  ASSERT_TRUE(description != nullptr);
  EXPECT_EQ(effectiveEnvOf(commandNamed(*description, "First")),
            std::vector<std::string>({"A=one", "B=first"}));
  EXPECT_EQ(effectiveEnvOf(commandNamed(*description, "Second")),
            std::vector<std::string>({"A=one", "B=second", "ONLY_SECOND=yes"}));
}

/// The point of the whole feature: rewriting a manifest to share a base must
/// not disturb any command's signature, or the first build after the change
/// rebuilds the world.
TEST(ShellCommandTest, signatureIsPreservedByConversion) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);

  auto expanded = loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "hi"]
    env:
      A: one
      B: two
      C: three
)");

  StubBuildFileDelegate factoredDelegate;
  TmpDir factoredTempDir(std::string(__FUNCTION__) + "-factored");
  auto factored = loadBuildFile(factoredDelegate, factoredTempDir, R"(
client:
  name: basic
env-bases:
  common:
    A: one
    B: unfactored
    C: three
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "hi"]
    env-base: common
    env:
      B: two
)");

  ASSERT_TRUE(expanded != nullptr);
  ASSERT_TRUE(factored != nullptr);

  auto* expandedCommand = commandNamed(*expanded, "Cmd");
  auto* factoredCommand = commandNamed(*factored, "Cmd");
  ASSERT_TRUE(expandedCommand != nullptr);
  ASSERT_TRUE(factoredCommand != nullptr);

  // Same effective environment...
  EXPECT_EQ(effectiveEnvOf(expandedCommand), effectiveEnvOf(factoredCommand));
  // ...and therefore the same signature.
  EXPECT_EQ(expandedCommand->getSignature(), factoredCommand->getSignature());
}

/// Changing a base has to reach the signatures of the commands using it,
/// otherwise a real environment change would go unnoticed.
TEST(ShellCommandTest, signatureFollowsTheBase) {
  StubBuildFileDelegate firstDelegate;
  TmpDir firstTempDir(std::string(__FUNCTION__) + "-first");
  auto first = loadBuildFile(firstDelegate, firstTempDir, R"(
client:
  name: basic
env-bases:
  common:
    A: one
commands:
  Cmd:
    tool: shell
    env-base: common
)");

  StubBuildFileDelegate secondDelegate;
  TmpDir secondTempDir(std::string(__FUNCTION__) + "-second");
  auto second = loadBuildFile(secondDelegate, secondTempDir, R"(
client:
  name: basic
env-bases:
  common:
    A: changed
commands:
  Cmd:
    tool: shell
    env-base: common
)");

  ASSERT_TRUE(first != nullptr);
  ASSERT_TRUE(second != nullptr);
  EXPECT_NE(commandNamed(*first, "Cmd")->getSignature(),
            commandNamed(*second, "Cmd")->getSignature());
}

/// Naming a base that was never declared is a diagnosable error, not a silently
/// empty environment.
TEST(ShellCommandTest, unknownBaseIsAnError) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);
  loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
env-bases:
  common:
    A: one
commands:
  Cmd:
    tool: shell
    env-base: nonesuch
)");

  ASSERT_FALSE(delegate.errors.empty());
  EXPECT_NE(delegate.errors[0].find("unknown environment base"),
            std::string::npos);
}

/// The grammar is positional: 'env-bases' has to precede the commands that
/// reference it.
TEST(ShellCommandTest, basesMustPrecedeCommands) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);
  loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
env-bases:
  common:
    A: one
)");

  EXPECT_FALSE(delegate.errors.empty());
}

/// Loads two manifests that differ only in the body of command 'Cmd' and returns
/// whether it ends up with the same signature in both.
static bool signatureIsUnchangedBy(const std::string& first,
                                   const std::string& second,
                                   const char* name) {
  StubBuildFileDelegate firstDelegate;
  TmpDir firstTempDir(std::string(name) + "-first");
  auto firstDescription = loadBuildFile(firstDelegate, firstTempDir, first);

  StubBuildFileDelegate secondDelegate;
  TmpDir secondTempDir(std::string(name) + "-second");
  auto secondDescription = loadBuildFile(secondDelegate, secondTempDir, second);

  EXPECT_TRUE(firstDescription != nullptr);
  EXPECT_TRUE(secondDescription != nullptr);
  if (!firstDescription || !secondDescription)
    return false;

  return commandNamed(*firstDescription, "Cmd")->getSignature() ==
         commandNamed(*secondDescription, "Cmd")->getSignature();
}

/// Arguments the client marked as not affecting the outputs do not contribute to
/// the signature, so changing one is not a reason to rerun the command.
TEST(ShellCommandTest, signatureIgnoresMarkedArgs) {
  EXPECT_TRUE(signatureIsUnchangedBy(R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/cc", "-index-store-path", "/tmp/a", "-c", "x.c"]
    signature-ignored-args: ["1", "2"]
)", R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/cc", "-index-store-path", "/tmp/b", "-c", "x.c"]
    signature-ignored-args: ["1", "2"]
)", __FUNCTION__));
}

/// ...but every other argument still does.
TEST(ShellCommandTest, signatureIncludesUnmarkedArgs) {
  EXPECT_FALSE(signatureIsUnchangedBy(R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/cc", "-index-store-path", "/tmp/a", "-c", "x.c"]
    signature-ignored-args: ["1", "2"]
)", R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/cc", "-index-store-path", "/tmp/a", "-c", "y.c"]
    signature-ignored-args: ["1", "2"]
)", __FUNCTION__));
}

/// Marking an argument is itself a change: a command that ignores an argument
/// must not collide with one that hashes it.
TEST(ShellCommandTest, signatureDependsOnWhichArgsAreIgnored) {
  EXPECT_FALSE(signatureIsUnchangedBy(R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/cc", "-index-store-path", "/tmp/a"]
    signature-ignored-args: ["1", "2"]
)", R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/cc", "-index-store-path", "/tmp/a"]
)", __FUNCTION__));
}

/// Client state that is not visible in the command line or environment reaches
/// the signature through 'additional-signature-data'.
TEST(ShellCommandTest, additionalSignatureDataReachesTheSignature) {
  EXPECT_FALSE(signatureIsUnchangedBy(R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "hi"]
    additional-signature-data: one
)", R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "hi"]
    additional-signature-data: two
)", __FUNCTION__));
}

/// Indices have to be sorted, so the signature walk can consume them alongside
/// the arguments.
TEST(ShellCommandTest, signatureIgnoredArgsMustAscend) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);
  loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "a", "b"]
    signature-ignored-args: ["2", "1"]
)");

  ASSERT_FALSE(delegate.errors.empty());
  EXPECT_NE(delegate.errors[0].find("out of order value"), std::string::npos);
}

/// A non-numeric index is a diagnosable error rather than a silently skipped
/// entry, which would weaken the signature without saying so.
TEST(ShellCommandTest, signatureIgnoredArgsMustBeNumeric) {
  StubBuildFileDelegate delegate;
  TmpDir tempDir(__FUNCTION__);
  loadBuildFile(delegate, tempDir, R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "a"]
    signature-ignored-args: ["-index-store-path"]
)");

  ASSERT_FALSE(delegate.errors.empty());
  EXPECT_NE(delegate.errors[0].find("invalid value"), std::string::npos);
}

/// A client-supplied 'signature' still replaces the computed one wholesale, so
/// existing manifests keep their current behavior.
TEST(ShellCommandTest, explicitSignatureOverridesEverything) {
  EXPECT_TRUE(signatureIsUnchangedBy(R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "hi"]
    signature: fixed
)", R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "there"]
    additional-signature-data: ignored
    signature: fixed
)", __FUNCTION__));
}

}
