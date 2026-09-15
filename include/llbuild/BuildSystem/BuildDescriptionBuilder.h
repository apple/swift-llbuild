//===- BuildDescriptionBuilder.h --------------------------------*- C++ -*-===//
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

#ifndef LLBUILD_BUILDSYSTEM_BUILDDESCRIPTIONBUILDER_H
#define LLBUILD_BUILDSYSTEM_BUILDDESCRIPTIONBUILDER_H

#include "llbuild/Basic/Compiler.h"
#include "llbuild/Basic/LLVM.h"
#include "llbuild/BuildSystem/BuildDescription.h"
#include "llbuild/BuildSystem/BuildFile.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <utility>

namespace llbuild {
namespace buildsystem {

class Command;
class Node;
class Target;
class Tool;

/// The file-system comparison mode a build system runs with, mirroring the
/// manifest's `client.file-system` property.
enum class FileSystemMode {
  /// Full stat comparison, including device and inode ("default").
  Full = 0,

  /// Ignore device and inode changes ("device-agnostic").
  DeviceAgnostic = 1,

  /// Compare contents by checksum only ("checksum-only").
  ChecksumOnly = 2,
};

/// Accumulates the pieces of a build graph and assembles the final
/// `BuildDescription`.
///
/// This is the single construction path for a build description. `BuildFile`
/// drives it from parsed YAML; embedders that already hold the graph in memory
/// drive it directly (see the `llb_buildsystem_description_builder_*` C API),
/// skipping the manifest entirely.
///
/// Both routes bottom out in the same `Tool::createCommand` and
/// `Command::configure*` calls, so they cannot disagree about how a command is
/// configured and therefore cannot disagree about its signature. That
/// property is what makes it safe to switch an incremental build between the
/// two without invalidating the database.
class BuildDescriptionBuilder {
  // DO NOT COPY
  BuildDescriptionBuilder(const BuildDescriptionBuilder&) LLBUILD_DELETED_FUNCTION;
  void operator=(const BuildDescriptionBuilder&) LLBUILD_DELETED_FUNCTION;

  /// The delegate used to create nodes and tools, intern strings, and report
  /// errors.
  BuildFileDelegate& delegate;

  /// The name reported as the origin of configuration diagnostics. For the YAML
  /// route this is the manifest path; for the in-memory route it is a
  /// placeholder identifying the embedder.
  std::string originName;

  BuildDescription::tool_set tools;
  BuildDescription::target_set targets;
  std::string defaultTarget;
  BuildDescription::node_set nodes;
  BuildDescription::environment_base_set environmentBases;
  BuildDescription::command_set commands;

  /// Whether `finalize()` should run ownership analysis over the command set.
  bool performOwnershipAnalysis = false;

  /// The file-system mode requested by the client. Not part of the graph; the
  /// embedder applies it to the build system itself.
  FileSystemMode fileSystemMode = FileSystemMode::Full;

public:
  BuildDescriptionBuilder(BuildFileDelegate& delegate, StringRef originName);
  ~BuildDescriptionBuilder();

  BuildFileDelegate& getDelegate() const { return delegate; }

  /// A configuration context that carries no source location, for callers that
  /// have no file to point at.
  ConfigureContext getContext();

  /// @name Nodes
  /// @{

  /// Look up `name`, creating the node via the delegate on first reference.
  ///
  /// `isImplicit` is false only when the node is being declared in its own
  /// right (a `nodes` entry) rather than merely referenced by a command.
  Node* getOrCreateNode(StringRef name, bool isImplicit);

  /// Configure an attribute on the named node, creating it if this is its first
  /// mention. Returns false if the node rejected the attribute, in which case
  /// the delegate has been sent a diagnostic.
  ///
  /// Declaring a node this way marks it non-implicit, matching a `nodes` entry
  /// in a manifest.
  bool configureNodeAttribute(StringRef nodeName, StringRef name,
                              StringRef value);
  bool configureNodeAttribute(StringRef nodeName, StringRef name,
                              ArrayRef<StringRef> values);
  bool configureNodeAttribute(
      StringRef nodeName, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values);

  /// @}
  /// @name Tools
  /// @{

  /// Look up `name`, asking the delegate to create the tool on first reference.
  /// Returns null if the delegate does not recognize it; the caller is
  /// responsible for reporting that.
  Tool* getOrCreateTool(StringRef name);

  /// @}
  /// @name Targets
  /// @{

  bool hasTarget(StringRef name) const;

  /// Create a target over the named nodes and hand it to the delegate.
  Target* addTarget(StringRef name, ArrayRef<StringRef> nodeNames);

  /// Set the default target. Returns false if no such target was declared.
  bool setDefaultTarget(StringRef name);

  /// @}
  /// @name Environment bases
  /// @{

  bool hasEnvironmentBase(StringRef name) const;

  EnvironmentBase* lookupEnvironmentBase(StringRef name);

  /// Declare a shared environment table. `bindings` are interned through the
  /// delegate, so callers need not keep the underlying storage alive.
  ///
  /// Returns null if a base of this name was already declared.
  EnvironmentBase* addEnvironmentBase(
      StringRef name, ArrayRef<std::pair<StringRef, StringRef>> bindings);

  /// @}
  /// @name Commands
  /// @{

  bool hasCommand(StringRef name) const;

  /// Create, but don't yet register,  a command for `toolName`.
  ///
  /// Returns null if the tool is unknown or refuses to create the command; the
  /// caller reports the specific diagnostic.
  std::unique_ptr<Command> createCommand(StringRef name, StringRef toolName);

  /// Configure `command`'s inputs.
  void configureCommandInputs(Command& command, ArrayRef<Node*> inputs,
                              const ConfigureContext& ctx);

  /// Configure `command`'s outputs, recording it as a producer of each.
  void configureCommandOutputs(Command& command, ArrayRef<Node*> outputs,
                               const ConfigureContext& ctx);

  /// Name-resolving conveniences for embedders that hold node names rather than
  /// `Node*`. Nodes are created on demand, as implicit references.
  void configureCommandInputs(Command& command, ArrayRef<StringRef> nodeNames,
                              const ConfigureContext& ctx);
  void configureCommandOutputs(Command& command, ArrayRef<StringRef> nodeNames,
                               const ConfigureContext& ctx);

  void configureCommandDescription(Command& command, StringRef description,
                                   const ConfigureContext& ctx);

  /// Forward a tool-specific attribute to `command`. Returns false if the
  /// command rejected it, in which case the delegate has been sent a
  /// diagnostic.
  bool configureCommandAttribute(Command& command, StringRef name,
                                 StringRef value, const ConfigureContext& ctx);
  bool configureCommandAttribute(Command& command, StringRef name,
                                 ArrayRef<StringRef> values,
                                 const ConfigureContext& ctx);
  bool configureCommandAttribute(
      Command& command, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values,
      const ConfigureContext& ctx);

  /// Point `command` at a previously declared environment base, whose bindings
  /// it inherits and may override key-by-key via its own `env` attribute.
  ///
  /// Returns false if no such base was declared, or if the command does not
  /// support environments.
  bool configureCommandEnvironmentBase(Command& command, StringRef baseName,
                                       const ConfigureContext& ctx);

  /// Register a fully-configured command and notify the delegate.
  void addCommand(StringRef name, std::unique_ptr<Command> command);

  /// @}

  bool getPerformOwnershipAnalysis() const { return performOwnershipAnalysis; }
  void setPerformOwnershipAnalysis(bool value) {
    performOwnershipAnalysis = value;
  }

  /// The file-system mode the client asked for. This configures the build system
  /// rather than the graph, so the embedder, not `finalize()`, is what
  /// applies it. Clients whose manifests declare a non-default mode must set it
  /// here too, or the two paths will disagree about which outputs are up to
  /// date.
  FileSystemMode getFileSystemMode() const { return fileSystemMode; }
  void setFileSystemMode(FileSystemMode mode) { fileSystemMode = mode; }

  /// Run any whole-graph analyses and hand back the assembled description.
  ///
  /// The builder is left empty. Returns null if ownership analysis failed, in
  /// which case the delegate has been sent the diagnostics.
  std::unique_ptr<BuildDescription> finalize();
};

}
}

#endif
