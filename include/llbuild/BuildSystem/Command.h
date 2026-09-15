//===- Command.h ------------------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildNode.h"

#ifndef LLBUILD_BUILDSYSTEM_COMMAND_H
#define LLBUILD_BUILDSYSTEM_COMMAND_H

namespace llbuild {

namespace core {

class TaskInterface;

}

namespace buildsystem {

/// The type used to pass parsed properties to the delegate.
typedef std::vector<std::pair<std::string, std::string>> property_list_type;

class BuildSystem;
class BuildKey;
class BuildValue;
class Command;
class EnvironmentBase;
class Node;

/// Context for information that may be needed for a configuration action.
//
// FIXME: This is currently commingled with the build file loading, even though
// it should ideally be possible to create a build description decoupled
// entirely from the build file representation.
struct ConfigureContext;

/// Abstract command definition used by the build file.
class Command : public basic::JobDescriptor {
  // DO NOT COPY
  Command(const Command&) LLBUILD_DELETED_FUNCTION;
  void operator=(const Command&) LLBUILD_DELETED_FUNCTION;
  Command &operator=(Command&& rhs) LLBUILD_DELETED_FUNCTION;
    
  std::string name;
public:
  explicit Command(StringRef name) : name(name) {}
  virtual ~Command();

  std::vector<BuildNode*> inputs;
  std::vector<BuildNode*> outputs;
  bool repairViaOwnershipAnalysis = false;

  StringRef getName() const { return name; }

  /// @name Command Information
  /// @{
  //
  // FIXME: These probably don't belong here, clients generally can just manage
  // the information from their commands directly and our predefined interfaces
  // won't necessarily match what they want. However, we use them now to allow
  // extracting generic status information from the builtin commands. An
  // alternate solution would be to simply expose those command classes directly
  // and provide some kind of dynamic dispatching mechanism (llvm::cast<>, for
  // example) over commands.

  /// Controls whether the default status reporting shows status for the
  /// command.
  virtual bool shouldShowStatus() { return true; }

  virtual StringRef getOrdinalName() const override { return getName(); }

  /// Get a short description of the command, for use in status reporting.
  virtual void getShortDescription(SmallVectorImpl<char> &result) const override = 0;
  
  /// Get a verbose description of the command, for use in status reporting.
  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override = 0;

  virtual basic::CommandSignature getSignature() const;
  
  /// @}

  /// @name File Loading
  /// @{

  /// Called by the build file loader to set the description.
  virtual void configureDescription(const ConfigureContext&,
                                    StringRef description) = 0;

  /// Called by the build file loader to pass the list of input nodes.
  virtual void configureInputs(const ConfigureContext&,
                               const std::vector<Node*>& inputs) = 0;

  /// Called by the build file loader to pass the list of output nodes.
  virtual void configureOutputs(const ConfigureContext&,
                                const std::vector<Node*>& outputs) = 0;
                               
  /// Called by the build file loader to configure a specified command property.
  virtual bool configureAttribute(const ConfigureContext&, StringRef name,
                                  StringRef value) = 0;
  /// Called by the build file loader to configure a specified command property.
  virtual bool configureAttribute(const ConfigureContext&, StringRef name,
                                  ArrayRef<StringRef> values) = 0;
  /// Called by the build file loader to configure a specified command property.
  virtual bool configureAttribute(
      const ConfigureContext&, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) = 0;

  /// Called by the build file loader to attach a shared environment table that
  /// the command's own environment bindings are layered over.
  ///
  /// Not pure virtual: only commands that have an environment can make sense of
  /// a base for one, and returning false lets the loader diagnose the rest.
  ///
  /// \returns True if the command accepted the base.
  virtual bool configureEnvironmentBase(const ConfigureContext&,
                                        const EnvironmentBase*) {
    return false;
  }

  /// @}

  /// @name Node Interfaces
  ///
  /// @description These are the interfaces which allow the build system to
  /// coordinate between the abstract command and node objects.
  //
  // FIXME: This feels awkward, maybe this isn't the right way to manage
  // this. However, we want the system to be able to provide the plumbing
  // between pluggable comands and nodes, so it feels like it has to live
  // somewhere.
  //
  /// @{
  
  /// Get the appropriate output for a particular node (known to be produced by
  /// this command) given the command's result.
  virtual BuildValue getResultForOutput(Node* node,
                                        const BuildValue& value) = 0;

  /// @}
  
  /// @name Command Execution{
  ///
  /// @description These APIs directly mirror the APIs available in the
  /// lower-level BuildEngine, but with additional services provided by the
  /// BuildSystem. See the BuildEngine documentation for more information.
  ///
  /// @{

  virtual bool isResultValid(BuildSystem& system, const BuildValue& value) = 0;
  
  virtual void start(BuildSystem& system, core::TaskInterface ti) = 0;

  virtual void providePriorValue(BuildSystem& system, core::TaskInterface ti,
                                 const BuildValue& value) = 0;

  virtual void provideValue(BuildSystem& system, core::TaskInterface,
                            uintptr_t inputID, const core::KeyType& key, const BuildValue& value) = 0;


  virtual bool isExternalCommand() const { return false; }

  /// The command should execute outside the execution lanes.
  virtual bool isDetached() const { return false; }

  virtual void addOutput(BuildNode* node) final {
    outputs.push_back(node);
    node->getProducers().push_back(this);
  }

  virtual const std::vector<BuildNode*>& getInputs() const final {
    return inputs;
  }
  
  virtual const std::vector<BuildNode*>& getOutputs() const final {
    return outputs;
  }

  typedef std::function<void (BuildValue&&)> ResultFn;

  /// Execute the command, and return the value.
  ///
  /// This method will always be executed on the build execution queue.
  ///
  /// Note that resultFn may be executed asynchronously on a separate thread.
  virtual void execute(BuildSystem& system, core::TaskInterface ti,
                       basic::QueueJobContext* context, ResultFn resultFn) = 0;
  
  /// @}
};

}
}

#endif
