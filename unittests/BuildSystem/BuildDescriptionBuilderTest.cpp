//===- unittests/BuildSystem/BuildDescriptionBuilderTest.cpp --------------===//
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

#include "llbuild/BuildSystem/BuildDescriptionBuilder.h"

#include "llbuild/Basic/FileSystem.h"
#include "llbuild/BuildSystem/BuildDescription.h"
#include "llbuild/BuildSystem/BuildKey.h"
#include "llbuild/BuildSystem/BuildNode.h"
#include "llbuild/BuildSystem/BuildSystem.h"
#include "llbuild/BuildSystem/Command.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "MockBuildSystemDelegate.h"
#include "StubBuildFileDelegate.h"
#include "TempDir.h"

#include "gtest/gtest.h"

#include <string>
#include <vector>

#ifndef _WIN32
#include <climits>
#include <unistd.h>
#endif

using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::buildsystem;
using namespace llbuild::unittests;
using namespace llvm;

namespace {

/// Assemble a description through the builder alone, without a build system.
///
/// This is the same entry point the in-memory C API drives, so what these tests
/// observe about the resulting commands is what an embedder gets.
std::unique_ptr<BuildDescription> buildDescription(
    StubBuildFileDelegate& delegate,
    llvm::function_ref<bool(BuildDescriptionBuilder&)> populate) {
  BuildDescriptionBuilder builder(delegate, "<in-memory>");
  if (!populate(builder))
    return nullptr;
  return builder.finalize();
}

/// A trivial two-command chain, constructed entirely through the builder -- no
/// manifest text anywhere:
///
///     command "make-a":  (no inputs)  ->  node <a>
///     command "make-b":  node <a>     ->  node <b>
///     target  "":        [ <b> ]
struct Chain {
  std::string aPath;
  std::string bPath;

  Chain(const TmpDir& tempDir) {
    SmallString<256> a{tempDir.str()};
    sys::path::append(a, "a");
    aPath = a.str().str();

    SmallString<256> b{tempDir.str()};
    sys::path::append(b, "b");
    bPath = b.str().str();
  }

  bool populate(BuildDescriptionBuilder& builder) const {
    auto ctx = builder.getContext();
    StringRef a[] = {aPath};
    StringRef b[] = {bPath};
    std::string aArgs = "touch \"" + aPath + "\"";
    std::string bArgs = "touch \"" + bPath + "\"";

    auto makeA = builder.createCommand("make-a", "shell");
    if (!makeA)
      return false;
    builder.configureCommandOutputs(*makeA, a, ctx);
    if (!builder.configureCommandAttribute(*makeA, "args", aArgs, ctx))
      return false;
    builder.addCommand("make-a", std::move(makeA));

    auto makeB = builder.createCommand("make-b", "shell");
    if (!makeB)
      return false;
    builder.configureCommandInputs(*makeB, a, ctx);
    builder.configureCommandOutputs(*makeB, b, ctx);
    if (!builder.configureCommandAttribute(*makeB, "args", bArgs, ctx))
      return false;
    builder.addCommand("make-b", std::move(makeB));

    builder.addTarget("", b);
    return builder.setDefaultTarget("");
  }
};

/// Building the default target resolves it to its member nodes, which pull in
/// the rest of the chain. This is the in-memory path end to end: no manifest is
/// written, read, or parsed.
TEST(BuildDescriptionBuilderTest, executesViaDefaultTarget) {
  TmpDir tempDir(__func__);
  Chain chain(tempDir);

  MockBuildSystemDelegate delegate;
  BuildSystem system(delegate, createLocalFileSystem());

  ASSERT_TRUE(system.loadDescription(
      [&](BuildDescriptionBuilder& builder) { return chain.populate(builder); },
      "<in-memory>"));

  auto fs = createLocalFileSystem();
  ASSERT_TRUE(fs->getFileInfo(chain.aPath).isMissing());
  ASSERT_TRUE(fs->getFileInfo(chain.bPath).isMissing());

  ASSERT_TRUE(system.build(StringRef("")));

  EXPECT_FALSE(fs->getFileInfo(chain.aPath).isMissing());
  EXPECT_FALSE(fs->getFileInfo(chain.bPath).isMissing());
}

/// Building a node key directly still honors the dependency edge: asking for
/// <b> runs "make-a" first.
TEST(BuildDescriptionBuilderTest, executesChain) {
  TmpDir tempDir(__func__);
  Chain chain(tempDir);

  MockBuildSystemDelegate delegate;
  BuildSystem system(delegate, createLocalFileSystem());

  ASSERT_TRUE(system.loadDescription(
      [&](BuildDescriptionBuilder& builder) { return chain.populate(builder); },
      "<in-memory>"));

  auto fs = createLocalFileSystem();
  ASSERT_TRUE(fs->getFileInfo(chain.aPath).isMissing());

  auto result = system.build(BuildKey::makeNode(chain.bPath));
  ASSERT_TRUE(result.hasValue());

  EXPECT_FALSE(fs->getFileInfo(chain.aPath).isMissing());
  EXPECT_FALSE(fs->getFileInfo(chain.bPath).isMissing());
}

/// A populate callback that fails must fail the load rather than install a
/// half-built graph.
TEST(BuildDescriptionBuilderTest, failedPopulateFailsTheLoad) {
  MockBuildSystemDelegate delegate;
  BuildSystem system(delegate, createLocalFileSystem());

  EXPECT_FALSE(system.loadDescription(
      [](BuildDescriptionBuilder&) { return false; }, "<in-memory>"));
}

/// Naming a tool the delegate does not recognize yields no command, which is how
/// an embedder learns to fail the load. The builder itself does not diagnose it,
/// since only the caller knows what to point at.
TEST(BuildDescriptionBuilderTest, unknownToolYieldsNoCommand) {
  StubBuildFileDelegate delegate;
  BuildDescriptionBuilder builder(delegate, "<in-memory>");

  EXPECT_TRUE(builder.createCommand("Cmd", "no-such-tool") == nullptr);
  EXPECT_TRUE(builder.createCommand("Cmd", "shell") != nullptr);
}

/// Node attributes go through `Node::configureAttribute`, so the node's own
/// accept/reject contract applies just as it does for a `nodes` entry in a
/// manifest.
TEST(BuildDescriptionBuilderTest, configuresNodeAttributes) {
  StringRef patterns[] = {"*.tmp", "*.log"};

  StubBuildFileDelegate delegate;
  auto description =
      buildDescription(delegate, [&](BuildDescriptionBuilder& builder) {
        return builder.configureNodeAttribute("out", "is-mutated", "true") &&
               builder.configureNodeAttribute(
                   "out", "content-exclusion-patterns", patterns);
      });

  ASSERT_TRUE(description != nullptr);
  EXPECT_TRUE(delegate.errors.empty());

  auto it = description->getNodes().find("out");
  ASSERT_TRUE(it != description->getNodes().end());
  auto* node = static_cast<BuildNode*>(it->second.get());
  EXPECT_TRUE(node->isMutated());
  EXPECT_EQ(node->contentExclusionPatterns().getValues(),
            std::vector<StringRef>({"*.tmp", "*.log"}));
}

/// An attribute the node rejects is reported, so an embedder that sends garbage
/// finds out instead of building against a silently ignored setting.
TEST(BuildDescriptionBuilderTest, rejectsUnknownNodeAttribute) {
  StubBuildFileDelegate delegate;
  BuildDescriptionBuilder builder(delegate, "<in-memory>");

  EXPECT_FALSE(builder.configureNodeAttribute("out", "bogus", "x"));
  EXPECT_FALSE(delegate.errors.empty());
}

/// Reach command 'Cmd' both ways -- parsed from `yaml`, and assembled by
/// `populate` through the builder -- and report whether llbuild hashes the two
/// identically.
///
/// The property the whole design rests on: a command configured through the
/// builder has the same signature as the same command parsed from a manifest. If
/// it did not, switching an incremental build between the two paths would rebuild
/// the world. So this compares the two directly rather than trusting that they
/// share code, because the signature is what the build database is keyed on.
///
/// The two front ends do converge on `Command::configureAttribute`, so what this
/// pins is not what an attribute *means* -- it is that the builder surface can
/// express everything the manifest grammar can, and that the embedder hands the
/// value over in the shape the command expects. A key that the loader parses but
/// that has no builder equivalent, or that one side passes as a scalar and the
/// other as a list, surfaces here as a mismatched signature rather than as a
/// spurious rebuild in the field.
static bool signaturesAgree(
    StringRef yaml, llvm::function_ref<bool(BuildDescriptionBuilder&)> populate,
    const char* name) {
  StubBuildFileDelegate parsedDelegate;
  TmpDir tempDir(name);
  auto parsed = loadBuildFile(parsedDelegate, tempDir, yaml);

  StubBuildFileDelegate builtDelegate;
  auto built = buildDescription(builtDelegate, populate);

  EXPECT_TRUE(parsed != nullptr);
  EXPECT_TRUE(built != nullptr);
  if (!parsed || !built)
    return false;

  auto* parsedCommand = commandNamed(*parsed, "Cmd");
  auto* builtCommand = commandNamed(*built, "Cmd");
  EXPECT_TRUE(parsedCommand != nullptr);
  EXPECT_TRUE(builtCommand != nullptr);
  if (!parsedCommand || !builtCommand)
    return false;

  EXPECT_EQ(effectiveEnvOf(parsedCommand), effectiveEnvOf(builtCommand));
  return parsedCommand->getSignature() == builtCommand->getSignature();
}

/// The baseline: everything a plain shell command carries.
TEST(BuildDescriptionBuilderTest, signatureMatchesTheBuildFilePath) {
  StringRef inputs[] = {"in"};
  StringRef outputs[] = {"out"};
  StringRef args[] = {"/bin/echo", "hi"};
  std::pair<StringRef, StringRef> env[] = {{"A", "one"}, {"B", "two"}};

  EXPECT_TRUE(signaturesAgree(R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    inputs: ["in"]
    outputs: ["out"]
    description: "run it"
    args: ["/bin/echo", "hi"]
    env:
      A: one
      B: two
    working-directory: "/tmp"
    allow-missing-inputs: true
)", [&](BuildDescriptionBuilder& builder) {
        auto ctx = builder.getContext();
        auto command = builder.createCommand("Cmd", "shell");
        if (!command)
          return false;

        builder.configureCommandInputs(*command, inputs, ctx);
        builder.configureCommandOutputs(*command, outputs, ctx);
        builder.configureCommandDescription(*command, "run it", ctx);

        bool ok = builder.configureCommandAttribute(*command, "args", args,
                                                    ctx) &&
                  builder.configureCommandAttribute(*command, "env", env, ctx) &&
                  builder.configureCommandAttribute(
                      *command, "working-directory", "/tmp", ctx) &&
                  builder.configureCommandAttribute(
                      *command, "allow-missing-inputs", "true", ctx);

        builder.addCommand("Cmd", std::move(command));
        return ok;
      }, __func__));
}

/// The indices are a list on both sides, and they subtract from the signature
/// rather than adding to it -- so a builder path that dropped them would agree
/// with a manifest that never named them, and disagree with one that did.
TEST(BuildDescriptionBuilderTest, signatureMatchesTheBuildFilePathForIgnoredArgs) {
  StringRef args[] = {"/bin/cc", "-index-store-path", "/tmp/a", "-c", "x.c"};
  StringRef ignored[] = {"1", "2"};

  EXPECT_TRUE(signaturesAgree(R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/cc", "-index-store-path", "/tmp/a", "-c", "x.c"]
    signature-ignored-args: ["1", "2"]
)", [&](BuildDescriptionBuilder& builder) {
        auto ctx = builder.getContext();
        auto command = builder.createCommand("Cmd", "shell");
        if (!command)
          return false;

        bool ok = builder.configureCommandAttribute(*command, "args", args,
                                                    ctx) &&
                  builder.configureCommandAttribute(
                      *command, "signature-ignored-args", ignored, ctx);

        builder.addCommand("Cmd", std::move(command));
        return ok;
      }, __func__));
}

/// Unlike the rest, this one is a scalar the client invents, so there is nothing
/// in `args` or `env` to fall back on if a path drops it.
TEST(BuildDescriptionBuilderTest,
     signatureMatchesTheBuildFilePathForAdditionalSignatureData) {
  StringRef args[] = {"/bin/echo", "hi"};

  EXPECT_TRUE(signaturesAgree(R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "hi"]
    additional-signature-data: "CLANG: 1700.0.13.3"
)", [&](BuildDescriptionBuilder& builder) {
        auto ctx = builder.getContext();
        auto command = builder.createCommand("Cmd", "shell");
        if (!command)
          return false;

        bool ok = builder.configureCommandAttribute(*command, "args", args,
                                                    ctx) &&
                  builder.configureCommandAttribute(
                      *command, "additional-signature-data",
                      "CLANG: 1700.0.13.3", ctx);

        builder.addCommand("Cmd", std::move(command));
        return ok;
      }, __func__));
}

/// A control for the three above: they would all pass just as happily if
/// `signaturesAgree` were comparing two commands that had been configured with
/// nothing at all. Here the manifest names an attribute the builder side omits,
/// which has to be caught.
TEST(BuildDescriptionBuilderTest, signatureParityDetectsADroppedAttribute) {
  StringRef args[] = {"/bin/echo", "hi"};

  EXPECT_FALSE(signaturesAgree(R"(
client:
  name: basic
commands:
  Cmd:
    tool: shell
    args: ["/bin/echo", "hi"]
    additional-signature-data: "CLANG: 1700.0.13.3"
)", [&](BuildDescriptionBuilder& builder) {
        auto ctx = builder.getContext();
        auto command = builder.createCommand("Cmd", "shell");
        if (!command)
          return false;

        bool ok = builder.configureCommandAttribute(*command, "args", args,
                                                    ctx);

        builder.addCommand("Cmd", std::move(command));
        return ok;
      }, __func__));
}

/// Factoring a command's environment into a shared base -- the point of
/// `addEnvironmentBase`, which lets an embedder send the shared bulk once instead
/// of once per command -- must not disturb its signature either.
TEST(BuildDescriptionBuilderTest, environmentBasePreservesTheSignature) {
  auto populate = [](bool factored) {
    return [factored](BuildDescriptionBuilder& builder) {
      auto ctx = builder.getContext();
      std::pair<StringRef, StringRef> full[] = {
          {"A", "one"}, {"B", "two"}, {"C", "three"}};
      std::pair<StringRef, StringRef> base[] = {
          {"A", "one"}, {"B", "unfactored"}, {"C", "three"}};
      std::pair<StringRef, StringRef> delta[] = {{"B", "two"}};

      auto command = builder.createCommand("Cmd", "shell");
      if (!command)
        return false;

      bool ok;
      if (factored) {
        ok = builder.addEnvironmentBase("common", base) != nullptr &&
             builder.configureCommandEnvironmentBase(*command, "common", ctx) &&
             builder.configureCommandAttribute(*command, "env", delta, ctx);
      } else {
        ok = builder.configureCommandAttribute(*command, "env", full, ctx);
      }

      builder.addCommand("Cmd", std::move(command));
      return ok;
    };
  };

  StubBuildFileDelegate expandedDelegate;
  auto expanded = buildDescription(expandedDelegate, populate(false));
  ASSERT_TRUE(expanded != nullptr);

  StubBuildFileDelegate factoredDelegate;
  auto factored = buildDescription(factoredDelegate, populate(true));
  ASSERT_TRUE(factored != nullptr);

  // Same effective environment, with the base's ordering preserved...
  EXPECT_EQ(effectiveEnvOf(commandNamed(*expanded, "Cmd")),
            std::vector<std::string>({"A=one", "B=two", "C=three"}));
  EXPECT_EQ(effectiveEnvOf(commandNamed(*expanded, "Cmd")),
            effectiveEnvOf(commandNamed(*factored, "Cmd")));
  // ...and therefore the same signature.
  EXPECT_EQ(commandNamed(*expanded, "Cmd")->getSignature(),
            commandNamed(*factored, "Cmd")->getSignature());
}

/// The manifest grammar is positional -- `env-bases` has to precede the commands
/// that reference it -- but that is a property of the parser, not the graph. An
/// embedder walking its own task list can declare a base at the point it first
/// needs one.
TEST(BuildDescriptionBuilderTest, environmentBaseNeedsNoDeclarationOrder) {
  std::pair<StringRef, StringRef> base[] = {{"A", "one"}};

  StubBuildFileDelegate delegate;
  auto description =
      buildDescription(delegate, [&](BuildDescriptionBuilder& builder) {
        auto ctx = builder.getContext();
        auto command = builder.createCommand("Cmd", "shell");
        if (!command)
          return false;

        // The command exists before the base it will inherit from does.
        if (!builder.addEnvironmentBase("common", base))
          return false;

        bool ok =
            builder.configureCommandEnvironmentBase(*command, "common", ctx);
        builder.addCommand("Cmd", std::move(command));
        return ok;
      });

  ASSERT_TRUE(description != nullptr);
  EXPECT_TRUE(delegate.errors.empty());
  EXPECT_EQ(effectiveEnvOf(commandNamed(*description, "Cmd")),
            std::vector<std::string>({"A=one"}));
}

/// Pointing a command at a base that was never declared is a diagnosable error,
/// not a silently empty environment.
TEST(BuildDescriptionBuilderTest, unknownEnvironmentBaseIsAnError) {
  StubBuildFileDelegate delegate;
  auto description =
      buildDescription(delegate, [](BuildDescriptionBuilder& builder) {
        auto command = builder.createCommand("Cmd", "shell");
        if (!command)
          return false;
        return builder.configureCommandEnvironmentBase(*command, "nonesuch",
                                                       builder.getContext());
      });

  EXPECT_TRUE(description == nullptr);
  ASSERT_FALSE(delegate.errors.empty());
  EXPECT_NE(delegate.errors[0].find("unknown environment base"),
            std::string::npos);
}

/// Declaring the same base twice is refused, so an embedder cannot change what a
/// base means out from under the commands already inheriting it.
TEST(BuildDescriptionBuilderTest, duplicateEnvironmentBaseIsRefused) {
  std::pair<StringRef, StringRef> bindings[] = {{"A", "one"}};

  StubBuildFileDelegate delegate;
  auto description =
      buildDescription(delegate, [&](BuildDescriptionBuilder& builder) {
        return builder.addEnvironmentBase("common", bindings) != nullptr &&
               builder.addEnvironmentBase("common", bindings) == nullptr;
      });

  ASSERT_TRUE(description != nullptr);
  EXPECT_EQ(description->getEnvironmentBases().size(), 1u);
}

/// A default target has to name a target that exists, mirroring the check the
/// manifest loader performs.
TEST(BuildDescriptionBuilderTest, rejectsUnknownDefaultTarget) {
  StringRef nodes[] = {"out"};

  StubBuildFileDelegate delegate;
  BuildDescriptionBuilder builder(delegate, "<in-memory>");

  EXPECT_FALSE(builder.setDefaultTarget("all"));
  builder.addTarget("all", nodes);
  EXPECT_TRUE(builder.setDefaultTarget("all"));
}

/// The file-system mode configures the build system rather than the graph, so
/// `finalize()` leaves it alone and the embedder applies it. This is the part of
/// the manifest's `client` section that has no in-memory analogue, and a client
/// that forgets it would disagree with its own manifest about which outputs are
/// up to date.
TEST(BuildDescriptionBuilderTest, recordsTheFileSystemMode) {
  StubBuildFileDelegate delegate;
  BuildDescriptionBuilder builder(delegate, "<in-memory>");

  EXPECT_EQ(builder.getFileSystemMode(), FileSystemMode::Full);
  builder.setFileSystemMode(FileSystemMode::DeviceAgnostic);

  ASSERT_TRUE(builder.finalize() != nullptr);
  EXPECT_EQ(builder.getFileSystemMode(), FileSystemMode::DeviceAgnostic);
}

/// Ownership analysis is a whole-graph pass, so it runs in `finalize()` -- and
/// only when the embedder asks for it. Here two commands claim overlapping
/// outputs, which the analysis rejects and a plain load accepts.
TEST(BuildDescriptionBuilderTest, ownershipAnalysisIsOptional) {
  auto populate = [](bool ownership) {
    return [ownership](BuildDescriptionBuilder& builder) {
      builder.setPerformOwnershipAnalysis(ownership);

      auto ctx = builder.getContext();
      StringRef directory[] = {"/tmp/dir"};
      StringRef subpath[] = {"/tmp/dir/file"};

      for (auto entry: {std::make_pair(StringRef("First"),
                                       ArrayRef<StringRef>(directory)),
                        std::make_pair(StringRef("Second"),
                                       ArrayRef<StringRef>(subpath))}) {
        auto command = builder.createCommand(entry.first, "shell");
        if (!command)
          return false;
        builder.configureCommandOutputs(*command, entry.second, ctx);
        if (!builder.configureCommandAttribute(
                *command, "repair-via-ownership-analysis", "true", ctx))
          return false;
        builder.addCommand(entry.first, std::move(command));
      }
      return true;
    };
  };

  StubBuildFileDelegate unanalyzedDelegate;
  EXPECT_TRUE(buildDescription(unanalyzedDelegate, populate(false)) != nullptr);

  StubBuildFileDelegate analyzedDelegate;
  EXPECT_TRUE(buildDescription(analyzedDelegate, populate(true)) == nullptr);
}

// MARK: - Builtin tools
//
// `shell` is not the only tool an embedder drives: ordering gates are `phony`,
// and directory and symlink tasks are `mkdir` and `symlink`. Those three are
// builtins of the build system rather than of the build file delegate, so they
// are out of reach of `StubBuildFileDelegate` and cannot be checked with
// `signaturesAgree`. These run a real build instead and check the effect on
// disk, which for `mkdir` and `symlink` is the stronger assertion anyway.

/// A `phony` command carries no attributes -- only its edges -- so what there is
/// to test is that ordering flows through it. "make-b" depends on the gate, the
/// gate depends on <a>, and only "make-a" can produce <a>.
TEST(BuildDescriptionBuilderTest, executesThroughAPhonyGate) {
  TmpDir tempDir(__func__);
  Chain chain(tempDir);

  MockBuildSystemDelegate delegate;
  BuildSystem system(delegate, createLocalFileSystem());

  ASSERT_TRUE(system.loadDescription(
      [&](BuildDescriptionBuilder& builder) {
        auto ctx = builder.getContext();
        StringRef a[] = {chain.aPath};
        StringRef b[] = {chain.bPath};
        StringRef gate[] = {"<gate>"};
        std::string aArgs = "touch \"" + chain.aPath + "\"";
        // Fails unless the gate held "make-b" back until <a> existed.
        std::string bArgs =
            "test -f \"" + chain.aPath + "\" && touch \"" + chain.bPath + "\"";

        auto makeA = builder.createCommand("make-a", "shell");
        if (!makeA)
          return false;
        builder.configureCommandOutputs(*makeA, a, ctx);
        if (!builder.configureCommandAttribute(*makeA, "args", aArgs, ctx))
          return false;
        builder.addCommand("make-a", std::move(makeA));

        auto phony = builder.createCommand("gate", "phony");
        if (!phony)
          return false;
        builder.configureCommandInputs(*phony, a, ctx);
        builder.configureCommandOutputs(*phony, gate, ctx);
        builder.addCommand("gate", std::move(phony));

        auto makeB = builder.createCommand("make-b", "shell");
        if (!makeB)
          return false;
        builder.configureCommandInputs(*makeB, gate, ctx);
        builder.configureCommandOutputs(*makeB, b, ctx);
        if (!builder.configureCommandAttribute(*makeB, "args", bArgs, ctx))
          return false;
        builder.addCommand("make-b", std::move(makeB));

        builder.addTarget("", b);
        return builder.setDefaultTarget("");
      },
      "<in-memory>"));

  ASSERT_TRUE(system.build(StringRef("")));

  auto fs = createLocalFileSystem();
  EXPECT_FALSE(fs->getFileInfo(chain.aPath).isMissing());
  EXPECT_FALSE(fs->getFileInfo(chain.bPath).isMissing());
}

/// `mkdir` takes no attributes either; its output node names the directory.
TEST(BuildDescriptionBuilderTest, executesMkdir) {
  TmpDir tempDir(__func__);

  SmallString<256> outer{tempDir.str()};
  sys::path::append(outer, "outer");
  SmallString<256> inner{outer};
  sys::path::append(inner, "inner");
  std::string outerPath = outer.str().str();
  std::string innerPath = inner.str().str();

  MockBuildSystemDelegate delegate;
  BuildSystem system(delegate, createLocalFileSystem());

  ASSERT_TRUE(system.loadDescription(
      [&](BuildDescriptionBuilder& builder) {
        auto ctx = builder.getContext();
        StringRef outerNode[] = {outerPath};
        StringRef innerNode[] = {innerPath};

        auto makeOuter = builder.createCommand("make-outer", "mkdir");
        if (!makeOuter)
          return false;
        builder.configureCommandOutputs(*makeOuter, outerNode, ctx);
        builder.addCommand("make-outer", std::move(makeOuter));

        // Nested, so the edge has to be honored for this one to succeed.
        auto makeInner = builder.createCommand("make-inner", "mkdir");
        if (!makeInner)
          return false;
        builder.configureCommandInputs(*makeInner, outerNode, ctx);
        builder.configureCommandOutputs(*makeInner, innerNode, ctx);
        builder.addCommand("make-inner", std::move(makeInner));

        builder.addTarget("", innerNode);
        return builder.setDefaultTarget("");
      },
      "<in-memory>"));

  ASSERT_TRUE(system.build(StringRef("")));

  EXPECT_TRUE(sys::fs::is_directory(outerPath));
  EXPECT_TRUE(sys::fs::is_directory(innerPath));
}

/// `symlink` is the one builtin here that takes an attribute: `contents` is the
/// link target, which is written verbatim and never resolved.
TEST(BuildDescriptionBuilderTest, executesSymlink) {
  TmpDir tempDir(__func__);

  SmallString<256> link{tempDir.str()};
  sys::path::append(link, "link");
  std::string linkPath = link.str().str();

  MockBuildSystemDelegate delegate;
  BuildSystem system(delegate, createLocalFileSystem());

  ASSERT_TRUE(system.loadDescription(
      [&](BuildDescriptionBuilder& builder) {
        auto ctx = builder.getContext();
        StringRef linkNode[] = {linkPath};

        auto command = builder.createCommand("make-link", "symlink");
        if (!command)
          return false;
        builder.configureCommandOutputs(*command, linkNode, ctx);
        if (!builder.configureCommandAttribute(*command, "contents",
                                               "somewhere-else", ctx))
          return false;
        builder.addCommand("make-link", std::move(command));

        builder.addTarget("", linkNode);
        return builder.setDefaultTarget("");
      },
      "<in-memory>"));

  ASSERT_TRUE(system.build(StringRef("")));

  // Read the link back rather than following it: "somewhere-else" deliberately
  // does not exist, so a `contents` that had been resolved would be visible as
  // a link that does.
  auto fs = createLocalFileSystem();
  EXPECT_FALSE(fs->getLinkInfo(linkPath).isMissing());
  EXPECT_TRUE(fs->getFileInfo(linkPath).isMissing());

#ifndef _WIN32
  char buffer[PATH_MAX];
  auto length = ::readlink(linkPath.c_str(), buffer, sizeof(buffer) - 1);
  ASSERT_NE(length, -1);
  buffer[length] = '\0';
  EXPECT_EQ(std::string(buffer), "somewhere-else");
#endif
}

/// An unknown attribute has to be rejected on a builtin exactly as it is on a
/// build file, or an embedder's typo becomes a silently dropped configuration.
TEST(BuildDescriptionBuilderTest, rejectsUnknownBuiltinToolAttribute) {
  MockBuildSystemDelegate delegate;
  BuildSystem system(delegate, createLocalFileSystem());

  EXPECT_FALSE(system.loadDescription(
      [&](BuildDescriptionBuilder& builder) {
        auto command = builder.createCommand("make-link", "symlink");
        if (!command)
          return false;
        return builder.configureCommandAttribute(*command, "contnets", "oops",
                                                 builder.getContext());
      },
      "<in-memory>"));
}

/// Every tool the in-memory API's first client asks for resolves through the
/// build system's builtins, without the delegate vending any of them.
TEST(BuildDescriptionBuilderTest, resolvesTheBuiltinTools) {
  MockBuildSystemDelegate delegate;
  BuildSystem system(delegate, createLocalFileSystem());

  EXPECT_TRUE(system.loadDescription(
      [&](BuildDescriptionBuilder& builder) {
        for (auto tool: {"shell", "phony", "mkdir", "symlink",
                         "stale-file-removal"}) {
          EXPECT_TRUE(builder.createCommand("Cmd", tool) != nullptr)
              << "no builtin tool: " << tool;
        }
        EXPECT_TRUE(builder.createCommand("Cmd", "no-such-tool") == nullptr);
        return true;
      },
      "<in-memory>"));
}

}
