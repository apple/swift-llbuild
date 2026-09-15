//===- unittests/BuildSystem/BuildDescriptionLoadPerfTest.cpp -------------===//
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
//
// How long it takes to get a build description into llbuild, measured over
// synthetic graphs shaped like the ones real clients hand us.
//
// The measurement is DISABLED_, so it does not run in CI, it reports numbers
// rather than asserting on them, and a test that fails on wall clock is a test
// that fails on a loaded machine. Run it deliberately:
//
//     swift build --target llbuildBuildSystemTests
//     .build/out/Products/Release/llbuildBuildSystemTests \
//         --gtest_also_run_disabled_tests \
//         --gtest_filter='BuildDescriptionLoadPerfTest.*'
//
// Build Release. A Debug measurement of this is not a measurement of anything.
//
// Every profile reports into one table. Each route is timed over
// `LLBUILD_PERF_ITERATIONS` runs (default 10) and reported as p50 and p90.
// `LLBUILD_PERF_SCALE` multiplies every command count; the defaults are sized
// so a full run takes a few seconds rather than a few minutes, which makes them
// smaller than the graphs they are modeled on. Scale up before reading anything
// into the absolute numbers. `LLBUILD_PERF_PROFILES` takes a comma-separated
// list of profile names, for iterating on one shape without paying for the rest.
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildDescriptionBuilder.h"

#include "llbuild/Basic/FileSystem.h"
#include "llbuild/BuildSystem/BuildDescription.h"
#include "llbuild/BuildSystem/BuildFile.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include "StubBuildFileDelegate.h"
#include "TempDir.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::buildsystem;
using namespace llbuild::unittests;
using namespace llvm;

namespace {

/// Vends a stub shell tool under *any* name.
///
/// Real manifests name a couple of dozen tools, most of them the client's own.
/// What a tool does is irrelevant to load cost, but how many commands there are
/// is not, so the profiles reproduce the command mix by name and let every one
/// of them come back as a `ShellCommand`.
class PerfBuildFileDelegate : public StubBuildFileDelegate {
public:
  std::unique_ptr<Tool> lookupTool(StringRef name) override {
    return llvm::make_unique<StubShellTool>(name);
  }
};

/// How a graph is shaped, in the dimensions that decide what loading it costs.
///
/// `webkit` below is measured from a real 517 MiB manifest rather than guessed
/// at; the numbers in its comment are that manifest's. Getting this shape right
/// matters more than it looks: the cost is dominated by the sheer *count* of
/// environment bindings, and a profile that models the same byte total as
/// fewer, longer values measures memcpy where the real thing measures interning.
struct Profile {
  const char* name;

  /// Commands in the graph, before `LLBUILD_PERF_SCALE`.
  size_t commandCount;

  /// How many of those carry an environment. In a real manifest this is a small
  /// fraction of the total -- the rest are gates, copies, and generated files.
  size_t commandsWithEnv;

  /// Inputs each command takes from the preceding level, and outputs it
  /// declares. Together these set the edge count.
  size_t inputsPerCommand;
  size_t outputsPerCommand;

  /// Path segments in a generated node name, which is what sets how long node
  /// names are.
  size_t pathDepth;

  /// Environment variables per command carrying one.
  size_t envVarsPerCommand;

  /// How many distinct *key sets* the environments are drawn from. Real
  /// environments repeat: thousands of commands, a hundred-odd key sets. This
  /// is what environment bases exist to exploit.
  size_t envKeySetCount;

  /// Commands per level, which sets how wide the DAG is.
  size_t levelWidth;

  /// Tool names to cycle through, reproducing a real manifest's command mix.
  ArrayRef<StringRef> tools;

  /// One line on what this profile is for, printed in the table legend.
  const char* note;
};

/// The shell tools a real manifest is mostly made of, in roughly the proportion
/// the measured WebKit manifest has them (10746 shell, 5513 file-copy, 5058
/// phony, 3269 auxiliary-file, and a long tail).
const StringRef webkitTools[] = {
  "shell", "shell", "file-copy", "shell", "phony", "auxiliary-file",
  "shell", "file-copy", "phony", "shell", "auxiliary-file", "mkdir",
};

const StringRef shellOnly[] = {"shell"};

/// Modeled on a measured WebKit manifest: 517 MiB, 25159 commands across 28
/// tools, of which 4102 carry an environment. Those environments hold 5.73M
/// bindings drawn from just 123 distinct key sets.  A median of 1216 variables
/// per command, with *short* values (median 9 bytes, mean 45). Environment is
/// 402 MiB of the file, node references 30 MiB, command lines 29 MiB.
///
/// Simplified in one way worth knowing about: every environment here is the
/// same size, where the real ones range from 1216 to 5138 variables. The
/// aggregate binding count matches; the distribution does not.
const Profile webkitProfile = {
  /*name=*/"webkit", /*commandCount=*/25000, /*commandsWithEnv=*/4100,
  /*inputsPerCommand=*/3, /*outputsPerCommand=*/2, /*pathDepth=*/4,
  /*envVarsPerCommand=*/1400, /*envKeySetCount=*/123, /*levelWidth=*/500,
  /*tools=*/webkitTools,
  /*note=*/"modeled on a measured 517 MiB WebKit manifest",
};

/// The opposite extreme, and a synthetic one: no environment at all, but many
/// commands with long output paths, so node names are the entire cost. Here to
/// keep an eye on the case where there is nothing for environment factoring to
/// do.
const Profile nodeHeavyProfile = {
  /*name=*/"node-heavy", /*commandCount=*/40000, /*commandsWithEnv=*/0,
  /*inputsPerCommand=*/4, /*outputsPerCommand=*/4, /*pathDepth=*/12,
  /*envVarsPerCommand=*/0, /*envKeySetCount=*/0, /*levelWidth=*/500,
  /*tools=*/shellOnly,
  /*note=*/"no environment at all; node names are the whole cost",
};

/// Neither extreme. Here to catch a change that wins big on one of the shapes
/// above by quietly costing the ordinary case.
const Profile balancedProfile = {
  /*name=*/"balanced", /*commandCount=*/5000, /*commandsWithEnv=*/5000,
  /*inputsPerCommand=*/2, /*outputsPerCommand=*/2, /*pathDepth=*/6,
  /*envVarsPerCommand=*/12, /*envKeySetCount=*/8, /*levelWidth=*/250,
  /*tools=*/shellOnly,
  /*note=*/"the ordinary case, as a regression guard",
};

const Profile* const allProfiles[] = {
  &webkitProfile, &nodeHeavyProfile, &balancedProfile,
};

/// Length of the `index`th generated environment value.
///
/// Reproduces the measured size mix, 57% under 16 bytes, 23% under 64, 19%
/// under 256, 1% larger, which averages out to the ~45 bytes the real
/// manifest has. A single average length would get the byte total right and the
/// allocation behavior wrong.
size_t envValueLength(size_t index) {
  auto bucket = index % 100;
  if (bucket < 57)
    return 9;
  if (bucket < 80)
    return 30;
  if (bucket < 99)
    return 130;
  return 700;
}

size_t perfScale() {
  if (const char* value = ::getenv("LLBUILD_PERF_SCALE")) {
    auto scale = ::atoi(value);
    if (scale > 0)
      return size_t(scale);
  }
  return 1;
}

size_t perfIterations() {
  if (const char* value = ::getenv("LLBUILD_PERF_ITERATIONS")) {
    auto iterations = ::atoi(value);
    if (iterations > 0)
      return size_t(iterations);
  }
  return 10;
}

/// Whether `LLBUILD_PERF_PROFILES` asks for this profile. Unset means all of
/// them, which is the usual way to run this.
bool profileSelected(const Profile& profile) {
  const char* value = ::getenv("LLBUILD_PERF_PROFILES");
  if (!value || !*value)
    return true;

  SmallVector<StringRef, 4> selected;
  StringRef(value).split(selected, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  for (auto name: selected) {
    if (name.trim() == profile.name)
      return true;
  }
  return false;
}

/// A synthetic graph, held in a form that can be written out as a build file
/// *or* replayed into a `BuildDescriptionBuilder`.
///
/// One model driving both is the point: the two routes are only comparable if
/// they are demonstrably carrying the same graph.
struct SyntheticGraph {
  struct Command {
    std::string name;
    StringRef tool;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::string args;

    /// Index into `keySets`, or `size_t(-1)` for no environment.
    size_t keySet = size_t(-1);
    std::vector<std::string> envValues;
  };

  /// The distinct environment key sets, shared by the commands that use them.
  std::vector<std::vector<std::string>> keySets;

  std::vector<Command> commands;
  std::vector<std::string> roots;

  static SyntheticGraph generate(const Profile& profile, size_t scale);

  /// Total environment bindings, which is the number the `webkit` profile is
  /// really calibrated against.
  size_t bindingCount() const;

  std::string toBuildFile() const;
  bool populate(BuildDescriptionBuilder& builder) const;
  bool populateWithEnvironmentBases(BuildDescriptionBuilder& builder) const;
};

std::string pathOf(StringRef stem, size_t index, size_t depth) {
  std::string path = "/tmp/llbuild-perf";
  for (size_t i = 0; i != depth; ++i)
    path += "/Intermediates.noindex-" + std::to_string((index + i) % 64);
  path += "/" + stem.str() + "-" + std::to_string(index) + ".o";
  return path;
}

size_t SyntheticGraph::bindingCount() const {
  size_t count = 0;
  for (const auto& command: commands)
    count += command.envValues.size();
  return count;
}

SyntheticGraph SyntheticGraph::generate(const Profile& profile, size_t scale) {
  SyntheticGraph graph;
  auto commandCount = profile.commandCount * scale;
  auto commandsWithEnv = profile.commandsWithEnv * scale;

  for (size_t set = 0; set != profile.envKeySetCount; ++set) {
    std::vector<std::string> keys;
    for (size_t i = 0; i != profile.envVarsPerCommand; ++i) {
      // Key sets overlap heavily, as real ones do, most of the names are
      // common, a few differ per set. Names run ~27 characters, matching the
      // measured median.
      keys.push_back("LLBUILD_PERF_ENV_VARIABLE_" + std::to_string(i) + "_" +
                     std::to_string(i % 8 == 0 ? set : 0));
    }
    graph.keySets.push_back(std::move(keys));
  }

  for (size_t index = 0; index != commandCount; ++index) {
    Command command;
    command.name = "command-" + std::to_string(index);
    command.tool = profile.tools[index % profile.tools.size()];

    for (size_t i = 0; i != profile.outputsPerCommand; ++i) {
      command.outputs.push_back(
          pathOf("out", index * profile.outputsPerCommand + i,
                 profile.pathDepth));
    }

    // Inputs are drawn from the previous level, which makes a wide DAG rather
    // than a chain, the shape a real build has.
    if (index >= profile.levelWidth) {
      const auto& previous = graph.commands[index - profile.levelWidth].outputs;
      for (size_t i = 0; i != profile.inputsPerCommand; ++i)
        command.inputs.push_back(previous[i % previous.size()]);
    }

    command.args = "/usr/bin/true " + command.name;

    // Only the first `commandsWithEnv` carry one; the rest are the gates,
    // copies, and generated files that make up the bulk of a real manifest.
    if (profile.envVarsPerCommand != 0 && index < commandsWithEnv) {
      command.keySet = index % profile.envKeySetCount;
      for (size_t i = 0; i != profile.envVarsPerCommand; ++i) {
        // Values mostly agree with the rest of the key set's members, so
        // factoring has something to remove and something to keep.
        auto distinguisher = (i % 16 == 0) ? index : command.keySet;
        std::string value = std::to_string(distinguisher) + "-";
        value.append(envValueLength(i), 'x');
        command.envValues.push_back(std::move(value));
      }
    }

    graph.commands.push_back(std::move(command));
  }

  // Everything the last level produces.
  for (size_t index = commandCount - std::min(commandCount, profile.levelWidth);
       index != commandCount; ++index) {
    for (const auto& output: graph.commands[index].outputs)
      graph.roots.push_back(output);
  }

  return graph;
}

void writeQuotedList(raw_ostream& os, const std::vector<std::string>& values) {
  os << "[";
  for (size_t i = 0; i != values.size(); ++i) {
    if (i != 0)
      os << ",";
    os << "\"" << values[i] << "\"";
  }
  os << "]";
}

std::string SyntheticGraph::toBuildFile() const {
  std::string text;
  raw_string_ostream os(text);

  os << "client:\n  name: perf\n\ntargets:\n  \"\": ";
  writeQuotedList(os, roots);
  os << "\n\ncommands:\n";

  for (const auto& command: commands) {
    os << "  \"" << command.name << "\":\n";
    os << "    tool: " << command.tool << "\n";
    if (!command.inputs.empty()) {
      os << "    inputs: ";
      writeQuotedList(os, command.inputs);
      os << "\n";
    }
    os << "    outputs: ";
    writeQuotedList(os, command.outputs);
    os << "\n";
    os << "    args: \"" << command.args << "\"\n";
    if (command.keySet != size_t(-1)) {
      os << "    env:\n";
      const auto& keys = keySets[command.keySet];
      for (size_t i = 0; i != keys.size(); ++i)
        os << "      " << keys[i] << ": \"" << command.envValues[i] << "\"\n";
    }
  }

  os.flush();
  return text;
}

bool SyntheticGraph::populate(BuildDescriptionBuilder& builder) const {
  auto ctx = builder.getContext();

  for (const auto& command: commands) {
    auto created = builder.createCommand(command.name, command.tool);
    if (!created)
      return false;

    std::vector<StringRef> inputs(command.inputs.begin(), command.inputs.end());
    std::vector<StringRef> outputs(command.outputs.begin(),
                                   command.outputs.end());
    builder.configureCommandInputs(*created, inputs, ctx);
    builder.configureCommandOutputs(*created, outputs, ctx);

    if (!builder.configureCommandAttribute(*created, "args", command.args, ctx))
      return false;

    if (command.keySet != size_t(-1)) {
      const auto& keys = keySets[command.keySet];
      std::vector<std::pair<StringRef, StringRef>> env;
      env.reserve(keys.size());
      for (size_t i = 0; i != keys.size(); ++i)
        env.emplace_back(keys[i], command.envValues[i]);
      if (!builder.configureCommandAttribute(*created, "env", env, ctx))
        return false;
    }

    builder.addCommand(command.name, std::move(created));
  }

  std::vector<StringRef> rootNodes(roots.begin(), roots.end());
  builder.addTarget("", rootNodes);
  return builder.setDefaultTarget("");
}

bool SyntheticGraph::populateWithEnvironmentBases(
    BuildDescriptionBuilder& builder) const {
  auto ctx = builder.getContext();

  // One base per key set, declared from the first command that uses it. Every
  // later member sends only the values that differ.
  std::vector<std::string> baseNames;
  std::vector<const Command*> baseSources(keySets.size(), nullptr);
  for (size_t set = 0; set != keySets.size(); ++set)
    baseNames.push_back("env-base-" + std::to_string(set));
  for (const auto& command: commands) {
    if (command.keySet != size_t(-1) && !baseSources[command.keySet])
      baseSources[command.keySet] = &command;
  }
  for (size_t set = 0; set != keySets.size(); ++set) {
    if (!baseSources[set])
      continue;
    const auto& keys = keySets[set];
    std::vector<std::pair<StringRef, StringRef>> bindings;
    bindings.reserve(keys.size());
    for (size_t i = 0; i != keys.size(); ++i)
      bindings.emplace_back(keys[i], baseSources[set]->envValues[i]);
    if (!builder.addEnvironmentBase(baseNames[set], bindings))
      return false;
  }

  for (const auto& command: commands) {
    auto created = builder.createCommand(command.name, command.tool);
    if (!created)
      return false;

    std::vector<StringRef> inputs(command.inputs.begin(), command.inputs.end());
    std::vector<StringRef> outputs(command.outputs.begin(),
                                   command.outputs.end());
    builder.configureCommandInputs(*created, inputs, ctx);
    builder.configureCommandOutputs(*created, outputs, ctx);

    if (!builder.configureCommandAttribute(*created, "args", command.args, ctx))
      return false;

    if (command.keySet != size_t(-1)) {
      if (!builder.configureCommandEnvironmentBase(
              *created, baseNames[command.keySet], ctx))
        return false;

      const auto& keys = keySets[command.keySet];
      const auto& base = *baseSources[command.keySet];
      std::vector<std::pair<StringRef, StringRef>> overrides;
      for (size_t i = 0; i != keys.size(); ++i) {
        if (command.envValues[i] != base.envValues[i])
          overrides.emplace_back(keys[i], command.envValues[i]);
      }
      if (!overrides.empty() &&
          !builder.configureCommandAttribute(*created, "env", overrides, ctx))
        return false;
    }

    builder.addCommand(command.name, std::move(created));
  }

  std::vector<StringRef> rootNodes(roots.begin(), roots.end());
  builder.addTarget("", rootNodes);
  return builder.setDefaultTarget("");
}

/// Milliseconds spent in `body`.
template <typename Fn>
double timed(Fn&& body) {
  auto start = std::chrono::steady_clock::now();
  body();
  std::chrono::duration<double, std::milli> elapsed =
      std::chrono::steady_clock::now() - start;
  return elapsed.count();
}

/// Sorted per-iteration timings for one route.
struct Samples {
  std::vector<double> sorted;

  double median() const {
    auto count = sorted.size();
    if (count % 2 != 0)
      return sorted[count / 2];
    return (sorted[count / 2 - 1] + sorted[count / 2]) / 2;
  }

  /// Nearest-rank, which for the small iteration counts here is the honest
  /// reading: the 9th of 10 samples, not an interpolation between two of them.
  double p90() const {
    auto rank = size_t(std::ceil(0.9 * double(sorted.size())));
    return sorted[std::min(rank, sorted.size()) - 1];
  }
};

/// Run `route` `iterations` times.
///
/// Only the load is on the clock: each iteration gets a fresh delegate, and
/// tearing down the previous iteration's description, which for the larger
/// profiles is hundreds of megabytes, happens outside it.
template <typename Route>
Samples measureRoute(size_t iterations, Route&& route) {
  Samples samples;
  for (size_t i = 0; i != iterations; ++i) {
    PerfBuildFileDelegate delegate;
    std::unique_ptr<BuildDescription> description;

    samples.sorted.push_back(timed([&] { description = route(delegate); }));

    if (!description) {
      ADD_FAILURE() << "route produced no description";
      break;
    }
  }
  std::sort(samples.sorted.begin(), samples.sorted.end());
  return samples;
}

/// One route's timings, held until every profile has run so that all of them
/// can be printed as a single table.
struct Row {
  std::string profile;
  std::string route;
  Samples samples;

  /// The `parse` median for this profile, which is what the speedup column is
  /// relative to.
  double baselineMedian;
};

/// What a generated profile turned out to be, for the legend above the table.
struct Shape {
  std::string profile;
  size_t commandCount;
  size_t bindingCount;
  double megabytes;
  const char* note;
};

/// Generate `profile`, load it every way we have, and append the timings.
///
/// Nothing is asserted beyond every route having produced a description at all.
/// See the file comment for why there are no timing assertions.
void measure(const Profile& profile, std::vector<Shape>& shapes,
             std::vector<Row>& rows) {
  auto scale = perfScale();
  auto iterations = perfIterations();
  auto graph = SyntheticGraph::generate(profile, scale);

  // The build file is written outside the measurement. What is being timed is
  // llbuild reading it, not the test producing it.
  TmpDir tempDir(std::string("perf-") + profile.name);
  SmallString<256> path(tempDir.str());
  sys::path::append(path, "build.llbuild");
  auto text = graph.toBuildFile();
  {
    std::error_code ec;
    raw_fd_ostream os(path, ec, sys::fs::F_Text);
    ASSERT_FALSE(ec);
    os << text;
  }

  shapes.push_back({profile.name, graph.commands.size(), graph.bindingCount(),
                    double(text.size()) / (1024 * 1024), profile.note});

  auto parse = measureRoute(iterations, [&](PerfBuildFileDelegate& delegate) {
    return BuildFile(path, delegate).load();
  });
  auto baseline = parse.median();
  rows.push_back({profile.name, "parse", std::move(parse), baseline});

  auto builder = measureRoute(iterations, [&](PerfBuildFileDelegate& delegate) {
    BuildDescriptionBuilder builder(delegate, "<perf>");
    return graph.populate(builder) ? builder.finalize() : nullptr;
  });
  rows.push_back({profile.name, "builder", std::move(builder), baseline});

  if (profile.envVarsPerCommand != 0) {
    auto factored =
        measureRoute(iterations, [&](PerfBuildFileDelegate& delegate) {
          BuildDescriptionBuilder builder(delegate, "<perf>");
          return graph.populateWithEnvironmentBases(builder)
                     ? builder.finalize()
                     : nullptr;
        });
    rows.push_back(
        {profile.name, "builder + bases", std::move(factored), baseline});
  }
}

void report(size_t scale, size_t iterations, const std::vector<Shape>& shapes,
            const std::vector<Row>& rows) {
  outs() << "\nBuild description load\n"
         << std::string(70, '=') << "\n"
         << "  " << iterations << " iterations, scale " << scale << "\n\n";

  for (const auto& shape: shapes) {
    outs() << "  " << left_justify(shape.profile, 12)
           << right_justify(std::to_string(shape.commandCount), 6)
           << " commands"
           << right_justify(std::to_string(shape.bindingCount), 10)
           << " env bindings"
           << right_justify(formatv("{0:F1}", shape.megabytes).str(), 8)
           << " MiB   " << shape.note << "\n";
  }

  outs() << "\n  " << left_justify("profile", 12)
         << left_justify("route", 18) << right_justify("p50", 12)
         << right_justify("p90", 12) << right_justify("vs parse", 11) << "\n"
         << "  " << std::string(65, '-') << "\n";

  StringRef previous;
  for (const auto& row: rows) {
    // A blank line between profiles, so the groups read as groups even though
    // every row carries its own label.
    if (!previous.empty() && previous != row.profile)
      outs() << "\n";
    previous = row.profile;

    outs() << "  " << left_justify(row.profile, 12)
           << left_justify(row.route, 18)
           << right_justify(formatv("{0:F1}", row.samples.median()).str(), 9)
           << " ms"
           << right_justify(formatv("{0:F1}", row.samples.p90()).str(), 9)
           << " ms"
           << right_justify(
                  formatv("{0:F2}x", row.baselineMedian / row.samples.median())
                      .str(),
                  11)
           << "\n";
  }

  outs() << "\n";
  outs().flush();
}

TEST(BuildDescriptionLoadPerfTest, DISABLED_load) {
  std::vector<Shape> shapes;
  std::vector<Row> rows;

  for (const auto* profile: allProfiles) {
    if (!profileSelected(*profile))
      continue;
    measure(*profile, shapes, rows);
    if (::testing::Test::HasFailure())
      return;
  }

  report(perfScale(), perfIterations(), shapes, rows);
}

/// Not a measurement: a correctness check on the generator itself, so a bad
/// profile cannot quietly make the numbers above meaningless. This one *does*
/// run in CI, over a graph small enough to be free.
TEST(BuildDescriptionLoadPerfTest, generatorProducesEquivalentGraphsBothWays) {
  // Small, but with the webkit profile's tool mix, so the equivalence being
  // checked covers commands that are not `shell`.
  Profile tiny = balancedProfile;
  tiny.commandCount = 24;
  tiny.commandsWithEnv = 16;
  tiny.levelWidth = 5;
  tiny.envKeySetCount = 3;
  tiny.envVarsPerCommand = 4;
  tiny.tools = webkitTools;

  auto graph = SyntheticGraph::generate(tiny, /*scale=*/1);

  TmpDir tempDir(__func__);
  PerfBuildFileDelegate parseDelegate;
  auto parsed = loadBuildFile(parseDelegate, tempDir, graph.toBuildFile());
  ASSERT_TRUE(parsed != nullptr);
  EXPECT_TRUE(parseDelegate.errors.empty());

  PerfBuildFileDelegate buildDelegate;
  BuildDescriptionBuilder builder(buildDelegate, "<perf>");
  ASSERT_TRUE(graph.populate(builder));
  auto built = builder.finalize();
  ASSERT_TRUE(built != nullptr);
  EXPECT_TRUE(buildDelegate.errors.empty());

  PerfBuildFileDelegate factoredDelegate;
  BuildDescriptionBuilder factoredBuilder(factoredDelegate, "<perf>");
  ASSERT_TRUE(graph.populateWithEnvironmentBases(factoredBuilder));
  auto factored = factoredBuilder.finalize();
  ASSERT_TRUE(factored != nullptr);
  EXPECT_TRUE(factoredDelegate.errors.empty());

  EXPECT_EQ(parsed->getCommands().size(), built->getCommands().size());
  EXPECT_EQ(parsed->getNodes().size(), built->getNodes().size());

  // The graphs agree command for command, and so do their signatures, which
  // is what makes a timing comparison between them mean anything.
  for (const auto& command: graph.commands) {
    auto* fromFile = commandNamed(*parsed, command.name);
    ASSERT_TRUE(fromFile != nullptr) << command.name;
    ASSERT_TRUE(commandNamed(*built, command.name) != nullptr) << command.name;
    ASSERT_TRUE(commandNamed(*factored, command.name) != nullptr)
        << command.name;

    EXPECT_EQ(fromFile->getSignature(),
              commandNamed(*built, command.name)->getSignature())
        << command.name;
    EXPECT_EQ(fromFile->getSignature(),
              commandNamed(*factored, command.name)->getSignature())
        << command.name;
  }
}

}
