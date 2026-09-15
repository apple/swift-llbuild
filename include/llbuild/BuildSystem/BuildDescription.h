//===- BuildDescription.h ---------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef LLBUILD_BUILDSYSTEM_BUILDDESCRIPTION_H
#define LLBUILD_BUILDSYSTEM_BUILDDESCRIPTION_H

#include "llbuild/Basic/Compiler.h"
#include "llbuild/Basic/ExecutionQueue.h"
#include "llbuild/Basic/Hashing.h"
#include "llbuild/Basic/LLVM.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llbuild {

namespace core {

class Task;

}

namespace buildsystem {

class Command;
class Node;
class Tool;

/// Context for information that may be needed for a configuration action.
//
// FIXME: This is currently commingled with the build file loading, even though
// it should ideally be possible to create a build description decoupled
// entirely from the build file representation.
struct ConfigureContext;

/// Each Target declares a name that can be used to reference it, and a list of
/// the top-level nodes which must be built to bring that target up to date.
class Target {
  /// The name of the target.
  std::string name;

  /// The list of nodes that should be computed to build this target.
  std::vector<Node*> nodes;

public:
  explicit Target(std::string name) : name(name) { }

  StringRef getName() const { return name; }

  std::vector<Node*>& getNodes() { return nodes; }
  const std::vector<Node*>& getNodes() const { return nodes; }
};

/// Abstract definition for a Node used by the build file.
class Node {
  // DO NOT COPY
  Node(const Node&) LLBUILD_DELETED_FUNCTION;
  void operator=(const Node&) LLBUILD_DELETED_FUNCTION;
  Node &operator=(Node&& rhs) LLBUILD_DELETED_FUNCTION;
    
  /// The name used to identify the node.
  std::string name;

  /// The list of commands which can produce this node.
  //
  // FIXME: Optimize for single entry list.
  std::vector<Command*> producers;
  
public:
  explicit Node(StringRef name) : name(name) {}
  virtual ~Node();

  StringRef getName() const { return name; }
  
  std::vector<Command*>& getProducers() { return producers; }

  const std::vector<Command*>& getProducers() const { return producers; }
  
  /// Called by the build file loader to configure a specified node property.
  virtual bool configureAttribute(const ConfigureContext&, StringRef name,
                                  StringRef value) = 0;
  /// Called by the build file loader to configure a specified node property.
  virtual bool configureAttribute(const ConfigureContext&, StringRef name,
                                  ArrayRef<StringRef> values) = 0;
  /// Called by the build file loader to configure a specified node property.
  virtual bool configureAttribute(
      const ConfigureContext&, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) = 0;
};

/// A table of environment bindings shared by many commands.
///
/// Large projects hand thousands of script commands the same multi-thousand
/// entry environment, differing in only a handful of keys. Declaring the common part once and having
/// commands name it keeps both the build file and the work of loading it
/// proportional to what actually varies, rather than to the number of commands
/// times the size of their settings table.
///
/// A command combines its base with its own bindings via
/// `ShellCommand::getEffectiveEnv`, which is only called when the command is
/// about to run. Nothing is composed at load time.
class EnvironmentBase {
  // DO NOT COPY
  EnvironmentBase(const EnvironmentBase&) LLBUILD_DELETED_FUNCTION;
  void operator=(const EnvironmentBase&) LLBUILD_DELETED_FUNCTION;

  /// The bindings, in declaration order.
  ///
  /// These reference interned strings, and so are valid for as long as the
  /// delegate that interned them.
  std::vector<std::pair<StringRef, StringRef>> bindings;

public:
  EnvironmentBase() {}

  std::vector<std::pair<StringRef, StringRef>>& getBindings() {
    return bindings;
  }

  const std::vector<std::pair<StringRef, StringRef>>& getBindings() const {
    return bindings;
  }
};

/// A complete description of a build.
class BuildDescription {
public:
  typedef llvm::StringMap<std::unique_ptr<EnvironmentBase>> environment_base_set;

  // FIXME: This is an inefficent map, the string is duplicated.
  typedef llvm::StringMap<std::unique_ptr<Node>> node_set;
  
  // FIXME: This is an inefficent map, the string is duplicated.
  typedef llvm::StringMap<std::unique_ptr<Target>> target_set;

  // FIXME: This is an inefficent map, the string is duplicated.
  typedef llvm::StringMap<std::unique_ptr<Command>> command_set;
  
  // FIXME: This is an inefficent map, the string is duplicated.
  typedef llvm::StringMap<std::unique_ptr<Tool>> tool_set;

private:
  /// The shared environment tables commands may reference by name.
  ///
  /// Commands hold bare `EnvironmentBase*` pointers into this set, so it has to
  /// outlive them. Members are destroyed in reverse declaration order, so
  /// declaring this before `commands` makes it the last of the two to go.
  environment_base_set environmentBases;

  node_set nodes;

  target_set targets;

  command_set commands;
  
  tool_set tools;

  /// The default target.
  std::string defaultTarget;

public:
  /// @name Accessors
  /// @{

  /// Get the set of shared environment tables for the file.
  environment_base_set& getEnvironmentBases() { return environmentBases; }

  /// Get the set of shared environment tables for the file.
  const environment_base_set& getEnvironmentBases() const {
    return environmentBases;
  }

  /// Get the set of declared nodes for the file.
  node_set& getNodes() { return nodes; }

  /// Get the set of declared nodes for the file.
  const node_set& getNodes() const { return nodes; }

  /// Get the set of declared targets for the file.
  target_set& getTargets() { return targets; }

  /// Get the set of declared targets for the file.
  const target_set& getTargets() const { return targets; }

  /// Get the default target.
  std::string& getDefaultTarget() { return defaultTarget; }

  /// Get the default target.
  const std::string& getDefaultTarget() const { return defaultTarget; }

  /// Get the set of declared commands for the file.
  command_set& getCommands() { return commands; }

  /// Get the set of declared commands for the file.
  const command_set& getCommands() const { return commands; }

  /// Get the set of all tools used by the file.
  tool_set& getTools() { return tools; }

  /// Get the set of all tools used by the file.
  const tool_set& getTools() const { return tools; }

  /// @}
  /// @name Construction Helpers.
  /// @{

  Node& addNode(std::unique_ptr<Node> value);

  Target& addTarget(std::unique_ptr<Target> value);

  Command& addCommand(std::unique_ptr<Command> value);

  Tool& addTool(std::unique_ptr<Tool> value);
  
  /// @}
};

}
}

#endif
