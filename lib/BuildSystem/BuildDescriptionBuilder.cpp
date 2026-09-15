//===-- BuildDescriptionBuilder.cpp ---------------------------------------===//
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

#include "llbuild/Basic/LLVM.h"
#include "llbuild/BuildSystem/BuildDescription.h"
#include "llbuild/BuildSystem/BuildNode.h"
#include "llbuild/BuildSystem/Command.h"
#include "llbuild/BuildSystem/Tool.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <map>
#include <vector>

using namespace llbuild;
using namespace llbuild::buildsystem;

namespace {

class OwnershipAnalysis {
  std::map<BuildNode*, Command*> includedPaths;
  std::map<BuildNode*, Command*> excludedPaths;
  BuildFileDelegate& fileDelegate;

public:
  std::vector<std::pair<BuildNode*, Command*>> outputNodesAndCommands;

  std::vector<std::pair<BuildNode*, Command*>> directoryInputNodesAndCommands;

  OwnershipAnalysis(const BuildDescription::command_set& commands, BuildFileDelegate& fileDelegate): fileDelegate(fileDelegate) {
    // Extract outputs and directory inputs of all commands
    for (auto it = commands.begin(); it != commands.end(); it++) {
      Command* command = (*it).getValue().get();
      for (auto output: command->getOutputs()) {
        if (!output->isVirtual()) {
          outputNodesAndCommands.push_back(std::pair<BuildNode*, Command*>(output, command));
        }
      }

      for (auto input: command->getInputs()) {
        if (input->isDirectory()) {
          directoryInputNodesAndCommands.push_back(std::pair<BuildNode*, Command*>(input, command));
        }
      }
    }

    // Sort paths according to length to ensure we assign owner to parent before assigning owner to its subpaths
    std::sort(outputNodesAndCommands.begin(),
              outputNodesAndCommands.end(),
              [](const std::pair<BuildNode*, Command*> pairA,
                 const std::pair<BuildNode*, Command*> pairB) -> bool {
      return pairA.first->getName().str().length() < pairB.first->getName().str().length();
    });
  }

  /// Establish ownerships
  bool establishOwnerships() {
    for (auto outputNodeAndCommand: outputNodesAndCommands) {
        if (outputNodeAndCommand.second->isExternalCommand() && outputNodeAndCommand.second->repairViaOwnershipAnalysis == true) {
          Command *owner = includedOwnerOf(outputNodeAndCommand.first->getName());
          if (owner == nullptr) {
            setOwner(outputNodeAndCommand.first, outputNodeAndCommand.second);
          } else if (owner == outputNodeAndCommand.second) {
            // A path and some of its subpaths are listed as output dependencies of a task.. Do nothing.
          } else {
            std::vector<Command*> conflictingProducers;
            conflictingProducers.push_back(outputNodeAndCommand.second);
            conflictingProducers.push_back(owner);
            fileDelegate.cannotLoadDueToMultipleProducers(outputNodeAndCommand.first, conflictingProducers);
            return false;
          }
        } else {
          setExcludedOwner(outputNodeAndCommand.first, outputNodeAndCommand.second);
        }
    }

    return true;
  }

  /// Check if node is unowned
  const bool isIncludedUnownedNode(const BuildNode* node) {
    return includedOwnerOf(node->getName()) == nullptr && excludedOwnerOf(node->getName()) == nullptr;
  }

  /// Set owner
  void setOwner(BuildNode* node, Command* command) {
    includedPaths[node] = command;
  }
  
  /// Set owner of a path that is produced by a command excluded from ownership analysis so we can distinguish it from an unowned path
  void setExcludedOwner(BuildNode* node, Command* command) {
    excludedPaths[node] = command;
  }

  /// Lookup included owner (a directory prefix of inputPath that is included in the analysis)
  Command* includedOwnerOf(StringRef inputPath) {
    auto it = std::find_if(includedPaths.begin(), includedPaths.end(), [inputPath](const std::pair<BuildNode*, Command*>& buildNodeAndCommand) -> bool {
      if (buildNodeAndCommand.first->getName().endswith("/")) {
        return inputPath.startswith(buildNodeAndCommand.first->getName());
      } else {
        return inputPath.startswith(buildNodeAndCommand.first->getName().str() + "/");
      }
    });

    if (it != includedPaths.end()) {
      return (*it).second;
    } else {
      return nullptr;
    }
  }

  /// Lookup owner
  Command* excludedOwnerOf(StringRef inputPath) {
    auto it = std::find_if(excludedPaths.begin(), excludedPaths.end(), [inputPath](const std::pair<BuildNode*, Command*>& buildNodeAndCommand) -> bool {
      // TODO: a good explanation of why we use "==" as opposed to "startswith"
      return inputPath == buildNodeAndCommand.first->getName();
    });

    if (it != excludedPaths.end()) {
      return (*it).second;
    } else {
      return nullptr;
    }
  }

  // Add input node to additional outputs of its owner
  //
  // [TaskB]
  //  |
  //  v
  // owned-directory/
  //      libX.fake-h
  //  ,-- libY.fake-h (ownership analysis will automatically amend this to outputs of TaskB)
  //  |   libZ.fake-h
  //  v
  // [TaskC]
  //  |
  //  v
  // libY-from-TaskC.fake-h
  //
  // This ensures TaskC will wait until TaskB is finished.
  void amendOutputOfOwnersWithConsumedSubpaths() {
    for (auto directoryInputNodeAndCommand: directoryInputNodesAndCommands) {
      Command *owner = includedOwnerOf(directoryInputNodeAndCommand.first->getName());
      if (owner != nullptr) {
        auto ownerOutputs = owner->getOutputs();
        if (std::find(ownerOutputs.begin(), ownerOutputs.end(), directoryInputNodeAndCommand.first) == ownerOutputs.end()) {
          if (owner->repairViaOwnershipAnalysis) {
            owner->addOutput(directoryInputNodeAndCommand.first);
          }
        }
      }
    }
  }

  //
  // unowned-directory/
  //  |  a.txt <-- TaskA
  //  |  b.txt <-- TaskB
  //  v
  // TaskC
  //
  // We should add "a.txt" and "b.txt" to mustScanAfterPaths of "unowned-directory/".
  // This ensures TaskC will wait until TaskA and TaskB are finished.
  void deferScanningUnownedInputsUntilSubpathsAvailable() {
    auto unownedDirectoryInputNodesAndConsumingCommands = std::vector<std::pair<BuildNode*, Command*>>();
    std::copy_if(directoryInputNodesAndCommands.begin(),
                 directoryInputNodesAndCommands.end(),
                 std::back_inserter(unownedDirectoryInputNodesAndConsumingCommands),
                 [this](const std::pair<BuildNode*, Command*> directoryInputNodeAndCommand) -> bool {
      return isIncludedUnownedNode(directoryInputNodeAndCommand.first) && directoryInputNodeAndCommand.second->repairViaOwnershipAnalysis;
    });

    // For each output node and its producing command (e.g. "unowned-directory/a.txt" and "TaskA"),
    // check if there exists an unowned node (e.g. "unowned-directory/" used by "TaskC") that is a parent of the produced node.
    // Only add "a.txt" to mustScanAfterPaths of "unowned-directory/" if TaskC is marked as "repairViaOwnershipAnalysis".
    for (auto outputNodeAndCommand: outputNodesAndCommands) {
      auto repairableUnownedNode =
        std::find_if(unownedDirectoryInputNodesAndConsumingCommands.begin(),
                     unownedDirectoryInputNodesAndConsumingCommands.end(),
                     [=](std::pair<BuildNode*, Command*> unownedDirectoryAndCommand) -> bool {
          return outputNodeAndCommand.first->getName().startswith(unownedDirectoryAndCommand.first->getName()) && outputNodeAndCommand.second->repairViaOwnershipAnalysis == true;
      });
      
      if (repairableUnownedNode != unownedDirectoryInputNodesAndConsumingCommands.end()) {
        (*repairableUnownedNode).first->mustScanAfterPaths.push_back(outputNodeAndCommand.first->getName());
      }
    }
  }
};
}

#pragma mark - BuildDescriptionBuilder

BuildDescriptionBuilder::BuildDescriptionBuilder(BuildFileDelegate& delegate,
                                                 StringRef originName)
    : delegate(delegate), originName(originName.str()) {}

BuildDescriptionBuilder::~BuildDescriptionBuilder() {}

ConfigureContext BuildDescriptionBuilder::getContext() {
  return ConfigureContext{ delegate, originName, BuildFileToken{nullptr, 0} };
}

Node* BuildDescriptionBuilder::getOrCreateNode(StringRef name,
                                               bool isImplicit) {
  auto it = nodes.find(name);
  if (it != nodes.end())
    return it->second.get();

  auto node = delegate.createNode(name, isImplicit);
  assert(node);
  auto result = node.get();
  nodes[name] = std::move(node);
  return result;
}

Tool* BuildDescriptionBuilder::getOrCreateTool(StringRef name) {
  auto it = tools.find(name);
  if (it != tools.end())
    return it->second.get();

  auto tool = delegate.lookupTool(name);
  if (!tool)
    return nullptr;

  auto result = tool.get();
  tools[name] = std::move(tool);
  return result;
}

bool BuildDescriptionBuilder::configureNodeAttribute(StringRef nodeName,
                                                     StringRef name,
                                                     StringRef value) {
  return getOrCreateNode(nodeName, /*isImplicit=*/false)
      ->configureAttribute(getContext(), name, value);
}

bool BuildDescriptionBuilder::configureNodeAttribute(
    StringRef nodeName, StringRef name, ArrayRef<StringRef> values) {
  return getOrCreateNode(nodeName, /*isImplicit=*/false)
      ->configureAttribute(getContext(), name, values);
}

bool BuildDescriptionBuilder::configureNodeAttribute(
    StringRef nodeName, StringRef name,
    ArrayRef<std::pair<StringRef, StringRef>> values) {
  return getOrCreateNode(nodeName, /*isImplicit=*/false)
      ->configureAttribute(getContext(), name, values);
}

bool BuildDescriptionBuilder::hasTarget(StringRef name) const {
  return targets.count(name) != 0;
}

Target* BuildDescriptionBuilder::addTarget(StringRef name,
                                           ArrayRef<StringRef> nodeNames) {
  auto target = llvm::make_unique<Target>(name.str());
  for (auto nodeName: nodeNames) {
    target->getNodes().push_back(getOrCreateNode(nodeName,
                                                 /*isImplicit=*/true));
  }

  auto result = target.get();
  delegate.loadedTarget(name, *target);
  targets[name] = std::move(target);
  return result;
}

bool BuildDescriptionBuilder::setDefaultTarget(StringRef name) {
  if (!hasTarget(name))
    return false;

  defaultTarget = name.str();
  delegate.loadedDefaultTarget(defaultTarget);
  return true;
}

bool BuildDescriptionBuilder::hasEnvironmentBase(StringRef name) const {
  return environmentBases.count(name) != 0;
}

EnvironmentBase* BuildDescriptionBuilder::lookupEnvironmentBase(
    StringRef name) {
  auto it = environmentBases.find(name);
  return it == environmentBases.end() ? nullptr : it->second.get();
}

EnvironmentBase* BuildDescriptionBuilder::addEnvironmentBase(
    StringRef name, ArrayRef<std::pair<StringRef, StringRef>> bindings) {
  if (hasEnvironmentBase(name))
    return nullptr;

  auto base = llvm::make_unique<EnvironmentBase>();
  auto& stored = base->getBindings();
  stored.reserve(bindings.size());
  for (const auto& binding: bindings) {
    // Intern, so the bindings outlive whatever storage the caller supplied.
    stored.emplace_back(delegate.getInternedString(binding.first),
                        delegate.getInternedString(binding.second));
  }

  auto result = base.get();
  environmentBases[name] = std::move(base);
  return result;
}

bool BuildDescriptionBuilder::hasCommand(StringRef name) const {
  return commands.count(name) != 0;
}

std::unique_ptr<Command> BuildDescriptionBuilder::createCommand(
    StringRef name, StringRef toolName) {
  auto tool = getOrCreateTool(toolName);
  if (!tool)
    return nullptr;

  return tool->createCommand(name);
}

void BuildDescriptionBuilder::configureCommandInputs(
    Command& command, ArrayRef<Node*> inputs, const ConfigureContext& ctx) {
  command.configureInputs(ctx, std::vector<Node*>(inputs.begin(),
                                                  inputs.end()));
}

void BuildDescriptionBuilder::configureCommandOutputs(
    Command& command, ArrayRef<Node*> outputs, const ConfigureContext& ctx) {
  for (auto output: outputs) {
    // Record the command as a producer of each of its outputs.
    output->getProducers().push_back(&command);
  }
  command.configureOutputs(ctx, std::vector<Node*>(outputs.begin(),
                                                   outputs.end()));
}

void BuildDescriptionBuilder::configureCommandInputs(
    Command& command, ArrayRef<StringRef> nodeNames,
    const ConfigureContext& ctx) {
  std::vector<Node*> resolved;
  resolved.reserve(nodeNames.size());
  for (auto name: nodeNames)
    resolved.push_back(getOrCreateNode(name, /*isImplicit=*/true));
  configureCommandInputs(command, resolved, ctx);
}

void BuildDescriptionBuilder::configureCommandOutputs(
    Command& command, ArrayRef<StringRef> nodeNames,
    const ConfigureContext& ctx) {
  std::vector<Node*> resolved;
  resolved.reserve(nodeNames.size());
  for (auto name: nodeNames)
    resolved.push_back(getOrCreateNode(name, /*isImplicit=*/true));
  configureCommandOutputs(command, resolved, ctx);
}

void BuildDescriptionBuilder::configureCommandDescription(
    Command& command, StringRef description, const ConfigureContext& ctx) {
  command.configureDescription(ctx, description);
}

bool BuildDescriptionBuilder::configureCommandAttribute(
    Command& command, StringRef name, StringRef value,
    const ConfigureContext& ctx) {
  return command.configureAttribute(ctx, name, value);
}

bool BuildDescriptionBuilder::configureCommandAttribute(
    Command& command, StringRef name, ArrayRef<StringRef> values,
    const ConfigureContext& ctx) {
  return command.configureAttribute(ctx, name, values);
}

bool BuildDescriptionBuilder::configureCommandAttribute(
    Command& command, StringRef name,
    ArrayRef<std::pair<StringRef, StringRef>> values,
    const ConfigureContext& ctx) {
  return command.configureAttribute(ctx, name, values);
}

bool BuildDescriptionBuilder::configureCommandEnvironmentBase(
    Command& command, StringRef baseName, const ConfigureContext& ctx) {
  auto base = lookupEnvironmentBase(baseName);
  if (!base) {
    ctx.error(Twine("unknown environment base '") + baseName + "'");
    return false;
  }

  if (!command.configureEnvironmentBase(ctx, base)) {
    ctx.error("'env-base' is not supported by this command");
    return false;
  }

  return true;
}

void BuildDescriptionBuilder::addCommand(StringRef name,
                                         std::unique_ptr<Command> command) {
  delegate.loadedCommand(name, *command);
  commands[name] = std::move(command);
}

std::unique_ptr<BuildDescription> BuildDescriptionBuilder::finalize() {
  if (performOwnershipAnalysis) {
    OwnershipAnalysis ownershipAnalysis(commands, delegate);
    if (!ownershipAnalysis.establishOwnerships())
      return nullptr;

    ownershipAnalysis.amendOutputOfOwnersWithConsumedSubpaths();
    ownershipAnalysis.deferScanningUnownedInputsUntilSubpathsAvailable();
  }

  auto description = llvm::make_unique<BuildDescription>();
  std::swap(description->getEnvironmentBases(), environmentBases);
  std::swap(description->getNodes(), nodes);
  std::swap(description->getTargets(), targets);
  std::swap(description->getDefaultTarget(), defaultTarget);
  std::swap(description->getCommands(), commands);
  std::swap(description->getTools(), tools);
  return description;
}
