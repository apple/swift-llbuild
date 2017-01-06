//===-- ExternalCommand.cpp -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/ExternalCommand.h"

#include "llbuild/Basic/Hashing.h"
#include "llbuild/BuildSystem/BuildExecutionQueue.h"
#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/BuildKey.h"
#include "llbuild/BuildSystem/BuildNode.h"
#include "llbuild/BuildSystem/BuildSystemCommandInterface.h"
#include "llbuild/BuildSystem/BuildValue.h"

#include "llbuild/Basic/FileInfo.h"
#include "llbuild/Basic/LLVM.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::buildsystem;

uint64_t ExternalCommand::getSignature() {
  // FIXME: Use a more appropriate hashing infrastructure.
  using llvm::hash_combine;
  llvm::hash_code code = hash_value(getName());
  for (const auto* input: inputs) {
    code = hash_combine(code, input->getName());
  }
  for (const auto* output: outputs) {
    code = hash_combine(code, output->getName());
  }
  code = hash_combine(code, allowMissingInputs);
  code = hash_combine(code, allowModifiedOutputs);
  code = hash_combine(code, alwaysOutOfDate);
  return size_t(code);
}

void ExternalCommand::configureDescription(const ConfigureContext&,
                                           StringRef value) {
  description = value;
}

void ExternalCommand::
configureInputs(const ConfigureContext&,
                const std::vector<Node*>& value) {
  inputs.reserve(value.size());
  for (auto* node: value) {
    inputs.emplace_back(static_cast<BuildNode*>(node));
  }
}

void ExternalCommand::
configureOutputs(const ConfigureContext&, const std::vector<Node*>& value) {
  outputs.reserve(value.size());
  for (auto* node: value) {
    outputs.emplace_back(static_cast<BuildNode*>(node));
  }
}

bool ExternalCommand::
configureAttribute(const ConfigureContext& ctx, StringRef name,
                   StringRef value) {
  if (name == "allow-missing-inputs") {
    if (value != "true" && value != "false") {
      ctx.error("invalid value: '" + value + "' for attribute '" +
                name + "'");
      return false;
    }
    allowMissingInputs = value == "true";
    return true;
  } else if (name == "allow-modified-outputs") {
    if (value != "true" && value != "false") {
      ctx.error("invalid value: '" + value + "' for attribute '" +
                name + "'");
      return false;
    }
    allowModifiedOutputs = value == "true";
    return true;
  } else if (name == "always-out-of-date") {
    if (value != "true" && value != "false") {
      ctx.error("invalid value: '" + value + "' for attribute '" +
                name + "'");
      return false;
    }
    alwaysOutOfDate = value == "true";
    return true;
  } else {
    ctx.error("unexpected attribute: '" + name + "'");
    return false;
  }
}
bool ExternalCommand::
configureAttribute(const ConfigureContext& ctx, StringRef name,
                   ArrayRef<StringRef> values) {
  ctx.error("unexpected attribute: '" + name + "'");
  return false;
}
bool ExternalCommand::configureAttribute(
    const ConfigureContext& ctx, StringRef name,
    ArrayRef<std::pair<StringRef, StringRef>> values) {
  ctx.error("unexpected attribute: '" + name + "'");
  return false;
}

BuildValue ExternalCommand::
getResultForOutput(Node* node, const BuildValue& value) {
  // If the value was a failed or cancelled command, propagate the failure.
  if (value.isFailedCommand() || value.isPropagatedFailureCommand() ||
      value.isCancelledCommand())
    return BuildValue::makeFailedInput();
  if (value.isSkippedCommand())
      return BuildValue::makeSkippedCommand();

  // Otherwise, we should have a successful command -- return the actual
  // result for the output.
  assert(value.isSuccessfulCommand());

  // If the node is virtual, the output is always a virtual input value.
  if (static_cast<BuildNode*>(node)->isVirtual()) {
    return BuildValue::makeVirtualInput();
  }
    
  // Find the index of the output node.
  //
  // FIXME: This is O(N). We don't expect N to be large in practice, but it
  // could be.
  auto it = std::find(outputs.begin(), outputs.end(), node);
  assert(it != outputs.end());
    
  auto idx = it - outputs.begin();
  assert(idx < value.getNumOutputs());

  auto& info = value.getNthOutputInfo((unsigned int)idx);
  if (info.isMissing())
    return BuildValue::makeMissingOutput();
    
  return BuildValue::makeExistingInput(info);
}
  
bool ExternalCommand::isResultValid(BuildSystem& system,
                                    const BuildValue& value) {
  // Treat the command as always out-of-date, if requested.
  if (alwaysOutOfDate)
    return false;
      
  // If the prior value wasn't for a successful command, recompute.
  if (!value.isSuccessfulCommand())
    return false;
    
  // If the command's signature has changed since it was built, rebuild.
  if (value.getCommandSignature() != getSignature())
    return false;

  // Check the timestamps on each of the outputs.
  for (unsigned long i = 0, e = outputs.size(); i != e; ++i) {
    auto* node = outputs[i];

    // Ignore virtual outputs.
    if (node->isVirtual())
      continue;
      
    // Rebuild if the output information has changed.
    //
    // We intentionally allow missing outputs here, as long as they haven't
    // changed. This is under the assumption that the commands themselves are
    // behaving correctly when they exit successfully, and that downstream
    // commands would diagnose required missing inputs.
    //
    // FIXME: CONSISTENCY: One consistency issue in this model currently is that
    // if the output was missing, then appears, nothing will remove it; that
    // results in an inconsistent build. What would be nice if we supported
    // per-edge annotations on whether an output was optional -- in that case we
    // could enforce and error on the missing output if not annotated, and we
    // could enable behavior to remove such output files if annotated prior to
    // running the command.
    auto info = node->getFileInfo(system.getDelegate().getFileSystem());
    if (value.getNthOutputInfo((unsigned int)i) != info)
      return false;
  }

  // Otherwise, the result is ok.
  return true;
}

void ExternalCommand::start(BuildSystemCommandInterface& bsci,
                            core::Task* task) {
  // Notify the client the command is preparing to run.
  bsci.getDelegate().commandPreparing(this);
    
  // Initialize the build state.
  skipValue = llvm::None;
  hasMissingInput = false;

  // Request all of the inputs.
  unsigned id = 0;
  for (auto it = inputs.begin(), ie = inputs.end(); it != ie; ++it, ++id) {
    bsci.taskNeedsInput(task, BuildKey::makeNode(*it), id);
  }
}

void ExternalCommand::providePriorValue(BuildSystemCommandInterface&,
                                        core::Task*,
                                        const BuildValue& value) {
  if (value.isSuccessfulCommand()) {
    hasPriorResult = true;
    priorResultCommandSignature = value.getCommandSignature();
  }
}

void ExternalCommand::provideValue(BuildSystemCommandInterface& bsci,
                                   core::Task*,
                                   uintptr_t inputID,
                                   const BuildValue& value) {
  // Process the input value to see if we should skip this command.

  // All direct inputs should be individual node values.
  assert(!value.hasMultipleOutputs());
  assert(value.isExistingInput() || value.isMissingInput() ||
         value.isMissingOutput() || value.isFailedInput() ||
         value.isVirtualInput()  || value.isSkippedCommand());

  // If the input should cause this command to skip, how should it skip?
  auto getSkipValueForInput = [&]() -> llvm::Optional<BuildValue> {
    // If the value is an existing or virtual input, we are always good.
    if (value.isExistingInput() || value.isVirtualInput())
      return llvm::None;

    // We explicitly allow running the command against a missing output, under
    // the expectation that responsibility for reporting this situation falls to
    // the command.
    //
    // FIXME: Eventually, it might be nice to harden the format so that we know
    // when an output was actually required versus optional.
    if (value.isMissingOutput())
      return llvm::None;

    // If the value is a missing input, but those are allowed, it is ok.
    if (value.isMissingInput()) {
      if (allowMissingInputs)
        return llvm::None;
      else
        return BuildValue::makePropagatedFailureCommand();
    }

    // Propagate failure.
    if (value.isFailedInput())
      return BuildValue::makePropagatedFailureCommand();

    // A skipped dependency doesn't cause this command to skip.
    if (value.isSkippedCommand())
        return llvm::None;

    llvm_unreachable("unexpected input");
  };

  // Check if we need to skip the command because of this input.
  auto skipValueForInput = getSkipValueForInput();
  if (skipValueForInput.hasValue()) {
    skipValue = std::move(skipValueForInput);
    if (value.isMissingInput()) {
      hasMissingInput = true;

      // FIXME: Design the logging and status output APIs.
      bsci.getDelegate().error(
          "", {}, (Twine("missing input '") + inputs[inputID]->getName() +
                   "' and no rule to build it"));
    }
  } else {
    // If there is a missing input file (from a successful command), we always
    // need to run the command.
    if (value.isMissingOutput())
      canUpdateIfNewer = false;
  }
}

bool ExternalCommand::canUpdateIfNewerWithResult(const BuildValue& result) {
  // Unless `allowModifiedOutputs` is specified, we always need to update if
  // ran.
  if (!allowModifiedOutputs)
    return false;

  // If it was specified, then we can update if all of our outputs simply exist.
  for (unsigned i = 0, e = result.getNumOutputs(); i != e; ++i) {
    const FileInfo& outputInfo = result.getNthOutputInfo(i);

    // If the output is missing, we need to rebuild.
    if (outputInfo.isMissing())
      return false;
  }
  return true;
}

BuildValue
ExternalCommand::computeCommandResult(BuildSystemCommandInterface& bsci) {
  // Capture the file information for each of the output nodes.
  //
  // FIXME: We need to delegate to the node here.
  SmallVector<FileInfo, 8> outputInfos;
  for (auto* node: outputs) {
    if (node->isVirtual()) {
      outputInfos.push_back(FileInfo{});
    } else {
      outputInfos.push_back(node->getFileInfo(
                                bsci.getDelegate().getFileSystem()));
    }
  }
  return BuildValue::makeSuccessfulCommand(outputInfos, getSignature());
}

void ExternalCommand::inputsAvailable(BuildSystemCommandInterface& bsci,
                                      core::Task* task) {
  // If the build should cancel, do nothing.
  if (bsci.getDelegate().isCancelled()) {
    bsci.taskIsComplete(task, BuildValue::makeCancelledCommand());
    return;
  }
    
  // If this command should be skipped, do nothing.
  if (skipValue.hasValue()) {
    // If this command had a failed input, treat it as having failed.
    if (hasMissingInput) {
      // FIXME: Design the logging and status output APIs.
      bsci.getDelegate().error(
          "", {}, (Twine("cannot build '") + outputs[0]->getName() +
                   "' due to missing input"));

      // Report the command failure.
      bsci.getDelegate().hadCommandFailure();
    }

    bsci.taskIsComplete(task, skipValue.getValue());
    return;
  }
  assert(!hasMissingInput);

  // If it is legal to simply update the command, then see if we can do so.
  if (canUpdateIfNewer &&
      hasPriorResult && priorResultCommandSignature == getSignature()) {
    BuildValue result = computeCommandResult(bsci);
    if (canUpdateIfNewerWithResult(result)) {
      bsci.taskIsComplete(task, result);
      return;
    }
  }
    
  // Suppress static analyzer false positive on generalized lambda capture
  // (rdar://problem/22165130).
#ifndef __clang_analyzer__
  auto fn = [this, &bsci=bsci, task](QueueJobContext* context) {
    // Notify the client the actual command body is going to run.
    bsci.getDelegate().commandStarted(this);

    // Create the directories for the directories containing file outputs.
    //
    // FIXME: Implement a shared cache for this, to reduce the number of
    // syscalls required to make this happen.
    for (auto* node: outputs) {
      if (!node->isVirtual()) {
        // Attempt to create the directory; we ignore errors here under the
        // assumption the command will diagnose the situation if necessary.
        //
        // FIXME: Need to use the filesystem interfaces.
        auto parent = llvm::sys::path::parent_path(node->getName());
        if (!parent.empty()) {
          (void) llvm::sys::fs::create_directories(parent);
        }
      }
    }
    
    // Invoke the external command.
    auto result = executeExternalCommand(bsci, task, context);
    
    // Notify the client the command is complete.
    bsci.getDelegate().commandFinished(this);
    
    // Process the result.
    if (!result) {
      bsci.getDelegate().hadCommandFailure();

      // If the command failed, the result is failure.
      bsci.taskIsComplete(task, BuildValue::makeFailedCommand());
      return;
    }

    // Otherwise, complete with a successful result.
    bsci.taskIsComplete(task, computeCommandResult(bsci));
  };
  bsci.addJob({ this, std::move(fn) });
#endif
}
