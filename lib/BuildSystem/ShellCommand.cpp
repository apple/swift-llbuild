//===-- ShellCommand.cpp --------------------------------------------------===//
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

#include "llbuild/BuildSystem/ShellCommand.h"

#include "llbuild/Basic/FileSystem.h"
#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/BuildKey.h"
#include "llbuild/Core/DependencyInfoParser.h"
#include "llbuild/Core/MakefileDepsParser.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>

using namespace llvm;
using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::core;
using namespace llbuild::buildsystem;

void ShellCommand::start(BuildSystem& system, TaskInterface ti) {
  // Resolve the plugin state.
  handler = system.resolveShellCommandHandler(this);

  // Delegate to handler, if used.
  if (handler) {
    handlerState = handler->start(ti, this);
  }

  this->ExternalCommand::start(system, ti);
}

CommandSignature ShellCommand::getSignature() const {
  CommandSignature signature = cachedSignature;
  if (!signature.isNull())
    return signature;

  auto code = ExternalCommand::getSignature();
  if (!signatureData.empty()) {
    code = code.combine(signatureData);
  } else {
    // Skip the arguments the client marked as not affecting the outputs, so that
    // e.g. moving an index store does not invalidate every compile. `args` is
    // hashed in order and `signatureIgnoredArgs` is sorted ascending, so this is
    // a single walk of both.
    auto nextIgnored = signatureIgnoredArgs.begin();
    const auto ignoredEnd = signatureIgnoredArgs.end();
    for (uint32_t i = 0, e = args.size(); i != e; ++i) {
      if (nextIgnored != ignoredEnd && *nextIgnored == i) {
        ++nextIgnored;
        continue;
      }
      code = code.combine(args[i]);
    }
    // Hash the effective environment rather than just this command's own
    // bindings, so that moving bindings into a shared base does not change any
    // command's signature.
    SmallVector<std::pair<StringRef, StringRef>, 16> effectiveEnv;
    getEffectiveEnv(effectiveEnv);
    for (const auto& entry: effectiveEnv) {
      code = code.combine(entry.first);
      code = code.combine(entry.second);
    }
    for (const auto& path: depsPaths) {
      code = code.combine(path);
    }
    code = code.combine(int(depsStyle));
    code = code.combine(int(inheritEnv));
    code = code.combine(int(canSafelyInterrupt));
    if (!additionalSignatureData.empty()) {
      code = code.combine(additionalSignatureData);
    }
  }
  signature = code;
  if (signature.isNull()) {
    signature = CommandSignature(1);
  }
  cachedSignature = signature;
  return signature;
}

void ShellCommand::getEffectiveEnv(
    SmallVectorImpl<std::pair<StringRef, StringRef>>& result) const {
  if (!envBase) {
    result.append(envOverrides.begin(), envOverrides.end());
    return;
  }

  const auto& bindings = envBase->getBindings();
  result.reserve(result.size() + bindings.size() + envOverrides.size());

  auto findOverride = [&](StringRef key) {
    return std::find_if(envOverrides.begin(), envOverrides.end(),
                        [&](const std::pair<StringRef, StringRef>& entry) {
                          return entry.first == key;
                        });
  };

  // Substitute overrides in place of the base's bindings, so the effective
  // environment keeps the base's ordering. Commands sharing a base overwhelmingly
  // override keys the base already defines, they were split out of an
  // environment that had the same shape, so this reproduces exactly the
  // environment the command would have declared inline.
  for (const auto& binding: bindings) {
    auto it = findOverride(binding.first);
    result.emplace_back(binding.first, it == envOverrides.end() ? binding.second
                                                                : it->second);
  }

  // Anything the base does not mention is appended.
  for (const auto& entry: envOverrides) {
    auto it = std::find_if(bindings.begin(), bindings.end(),
                           [&](const std::pair<StringRef, StringRef>& binding) {
                             return binding.first == entry.first;
                           });
    if (it == bindings.end())
      result.push_back(entry);
  }
}

bool ShellCommand::processDiscoveredDependencies(BuildSystem& system,
                                                 core::TaskInterface ti,
                                                 QueueJobContext* context) {
  // It is an error if the dependencies style is not specified.
  //
  // FIXME: Diagnose this sooner.
  if (depsStyle == DepsStyle::Unused) {
    system.getDelegate().commandHadError(
        this, "missing required 'deps-style' specifier");
    return false;
  }

  for (const auto& depsPath: depsPaths) {
    // Read the dependencies file.
    std::unique_ptr<llvm::MemoryBuffer> input;
    if (llvm::sys::path::is_absolute(depsPath)) {
      input = system.getFileSystem().getFileContents(depsPath);
    } else {
      SmallString<PATH_MAX> absPath = StringRef(workingDirectory);
      llvm::sys::path::append(absPath, depsPath);
      llvm::sys::fs::make_absolute(absPath);
      input = system.getFileSystem().getFileContents(StringRef(absPath));
    }
    if (!input) {
      system.getDelegate().commandHadError(
          this, "unable to open dependencies file (" + depsPath + ")");
      return false;
    }

    switch (depsStyle) {
    case DepsStyle::Unused:
      assert(0 && "unreachable");
      break;

    case DepsStyle::Makefile:
      if (!processMakefileDiscoveredDependencies(
              system, ti, context, depsPath, input.get(), false))
        return false;
      continue;

    case DepsStyle::DependencyInfo:
      if (!processDependencyInfoDiscoveredDependencies(
              system, ti, context, depsPath, input.get()))
        return false;
      continue;

    case DepsStyle::MakefileIgnoringSubsequentOutputs:
      if (!processMakefileDiscoveredDependencies(
              system, ti, context, depsPath, input.get(), true))
        return false;
      continue;
    }
      
    llvm::report_fatal_error("unknown dependencies style");
  }

  return true;
}

bool ShellCommand::processMakefileDiscoveredDependencies(BuildSystem& system,
                                                         TaskInterface ti,
                                                         QueueJobContext* context,
                                                         StringRef depsPath,
                                                         llvm::MemoryBuffer* input,
                                                         bool ignoreSubsequentOutputs) {
  // Parse the output.
  //
  // We just ignore the rule, and add any dependency that we encounter in the
  // file.
  struct DepsActions : public core::MakefileDepsParser::ParseActions {
    BuildSystem& system;
    TaskInterface ti;
    ShellCommand* command;
    StringRef depsPath;
    unsigned numErrors{0};

    DepsActions(BuildSystem& system, TaskInterface ti,
                ShellCommand* command, StringRef depsPath)
        : system(system), ti(ti), command(command), depsPath(depsPath) {}

    virtual void error(StringRef message, uint64_t position) override {
      std::string msg;
      raw_string_ostream msgStream(msg);
      msgStream << "error reading dependency file '" << depsPath << "': "
          << message << " at position " << position;
      system.getDelegate().commandHadError(command, msgStream.str());
      ++numErrors;
    }

    virtual void actOnRuleDependency(StringRef dependency,
                                     StringRef unescapedWord) override {
      if (llvm::sys::path::is_absolute(unescapedWord)) {
        ti.discoveredDependency(BuildKey::makeNode(unescapedWord).toData());
        system.getDelegate().commandFoundDiscoveredDependency(command, unescapedWord, DiscoveredDependencyKind::Input);
        return;
      }

      // Generate absolute path
      //
      // NOTE: This is making the assumption that relative paths coming in a
      // dependency file are in relation to the explictly set working
      // directory, or the current working directory when it has not been set.
      SmallString<PATH_MAX> absPath = StringRef(command->workingDirectory);
      llvm::sys::path::append(absPath, unescapedWord);
      llvm::sys::fs::make_absolute(absPath);

      ti.discoveredDependency(BuildKey::makeNode(absPath).toData());
      system.getDelegate().commandFoundDiscoveredDependency(command, absPath, DiscoveredDependencyKind::Input);
    }

    virtual void actOnRuleStart(StringRef name,
                                const StringRef unescapedWord) override {}

    virtual void actOnRuleEnd() override {}
  };

  DepsActions actions(system, ti, this, depsPath);
  core::MakefileDepsParser(input->getBuffer(), actions, ignoreSubsequentOutputs).parse();
  return actions.numErrors == 0;
}

bool
ShellCommand::processDependencyInfoDiscoveredDependencies(BuildSystem& system,
                                                          TaskInterface ti,
                                                          QueueJobContext* context,
                                                          StringRef depsPath,
                                                          llvm::MemoryBuffer* input) {
  // Parse the output.
  //
  // We just ignore the rule, and add any dependency that we encounter in the
  // file.
  struct DepsActions : public core::DependencyInfoParser::ParseActions {
    BuildSystem& system;
    TaskInterface ti;
    ShellCommand* command;
    StringRef depsPath;
    unsigned numErrors{0};

    DepsActions(BuildSystem& system, TaskInterface ti,
                ShellCommand* command, StringRef depsPath)
        : system(system), ti(ti), command(command), depsPath(depsPath) {}

    virtual void error(const char* message, uint64_t position) override {
      system.getDelegate().commandHadError(
          command, ("error reading dependency file '" + depsPath.str() +
                    "': " + std::string(message)));
      ++numErrors;
    }

    // Ignore everything but actual inputs.
    virtual void actOnVersion(StringRef) override { }
    virtual void actOnMissing(StringRef path) override {
      system.getDelegate().commandFoundDiscoveredDependency(command, path, DiscoveredDependencyKind::Missing);
    }
    virtual void actOnOutput(StringRef path) override {
      system.getDelegate().commandFoundDiscoveredDependency(command, path, DiscoveredDependencyKind::Output);
    }
    virtual void actOnInput(StringRef path) override {
      ti.discoveredDependency(BuildKey::makeNode(path).toData());
      system.getDelegate().commandFoundDiscoveredDependency(command, path, DiscoveredDependencyKind::Input);
    }
  };

  DepsActions actions(system, ti, this, depsPath);
  core::DependencyInfoParser(input->getBuffer(), actions).parse();
  return actions.numErrors == 0;
}

bool ShellCommand::configureAttribute(const ConfigureContext& ctx, StringRef name,
                                      StringRef value) {
  if (name == "args") {
    // When provided as a scalar string, we default to executing using the
    // shell.
    args.clear();
#if defined(_WIN32)
    args.push_back(ctx.getDelegate().getInternedString(
                       "C:\\windows\\system32\\cmd.exe"));
    args.push_back(ctx.getDelegate().getInternedString("/C"));
#else
    args.push_back(ctx.getDelegate().getInternedString(DefaultShellPath));
    args.push_back(ctx.getDelegate().getInternedString("-c"));
#endif
    args.push_back(ctx.getDelegate().getInternedString(value));
  } else if (name == "signature") {
    signatureData = value;
  } else if (name == "additional-signature-data") {
    additionalSignatureData = value;
  } else if (name == "deps") {
    depsPaths.clear();
    depsPaths.emplace_back(value);
  } else if (name == "deps-style") {
    if (value == "makefile") {
      depsStyle = DepsStyle::Makefile;
    } else if (value == "dependency-info") {
      depsStyle = DepsStyle::DependencyInfo;
    } else if (value == "makefile-ignoring-subsequent-outputs") {
      depsStyle = DepsStyle::MakefileIgnoringSubsequentOutputs;
    } else {
      ctx.error("unknown 'deps-style': '" + value + "'");
      return false;
    }
    return true;
  } else if (name == "can-safely-interrupt") {
    if (value != "true" && value != "false") {
      ctx.error("invalid value: '" + value + "' for attribute '" +
                name + "'");
      return false;
    }
    canSafelyInterrupt = value == "true";
  } else if (name == "inherit-env") {
    if (value != "true" && value != "false") {
      ctx.error("invalid value: '" + value + "' for attribute '" +
                name + "'");
      return false;
    }
    inheritEnv = value == "true";
  } else if (name == "working-directory") {
    // Ensure the working directory is absolute. This will make sure any
    // relative directories are interpreted as relative to the CWD at the time
    // the rule is defined.
    SmallString<PATH_MAX> wd = value;
    llvm::sys::fs::make_absolute(wd);
    workingDirectory = StringRef(wd);
  } else if (name == "control-enabled") {
    if (value != "true" && value != "false") {
      ctx.error("invalid value: '" + value + "' for attribute '" +
                name + "'");
      return false;
    }
    controlEnabled = value == "true";
  } else {
    return ExternalCommand::configureAttribute(ctx, name, value);
  }

  return true;
}
  
bool ShellCommand::configureAttribute(const ConfigureContext& ctx, StringRef name,
                                              ArrayRef<StringRef> values) {
  if (name == "args") {
    // Diagnose missing arguments.
    if (values.empty()) {
      ctx.error("invalid arguments for command '" + getName() + "'");
      return false;
    }

    args.clear();
    args.reserve(values.size());
    for (auto arg: values) {
      args.emplace_back(ctx.getDelegate().getInternedString(arg));
    }
  } else if (name == "deps") {
    depsPaths.clear();
    depsPaths.insert(depsPaths.begin(), values.begin(), values.end());
  } else if (name == "signature-ignored-args") {
    // Indices into `args`, as decimal strings, strictly ascending. Indices past
    // the end of the argument list are permitted but have no effect, so this
    // attribute does not have to be configured after `args`.
    signatureIgnoredArgs.clear();
    signatureIgnoredArgs.reserve(values.size());
    for (auto value: values) {
      uint32_t index;
      if (value.getAsInteger(10, index)) {
        ctx.error("invalid value: '" + value.str() + "' for attribute '" +
                  name.str() + "'");
        return false;
      }
      if (!signatureIgnoredArgs.empty() && index <= signatureIgnoredArgs.back()) {
        ctx.error("out of order value: '" + value.str() + "' for attribute '" +
                  name.str() + "'");
        return false;
      }
      signatureIgnoredArgs.push_back(index);
    }
  } else {
    return ExternalCommand::configureAttribute(ctx, name, values);
  }

  return true;
}

bool ShellCommand::configureAttribute(
    const ConfigureContext& ctx, StringRef name,
    ArrayRef<std::pair<StringRef, StringRef>> values) {
  if (name == "env") {
    envOverrides.clear();
    envOverrides.reserve(values.size());
    for (const auto& entry: values) {
      envOverrides.emplace_back(
          std::make_pair(
              ctx.getDelegate().getInternedString(entry.first),
              ctx.getDelegate().getInternedString(entry.second)));
    }
  } else {
    return ExternalCommand::configureAttribute(ctx, name, values);
  }

  return true;
}

bool ShellCommand::configureEnvironmentBase(const ConfigureContext& ctx,
                                            const EnvironmentBase* base) {
  envBase = base;
  return true;
}

void ShellCommand::executeExternalCommand(
    BuildSystem& system,
    TaskInterface ti,
    QueueJobContext* context,
    llvm::Optional<ProcessCompletionFn> completionFn) {
  auto commandCompletionFn = [this, &system, ti, completionFn](ProcessResult result) mutable {
    if (result.status != ProcessStatus::Succeeded) {
      // If the command failed, there is no need to gather dependencies.
      if (completionFn.hasValue())
        completionFn.getValue()(result);
      return;
    }

    // Collect the discovered dependencies, if used.
    if (!depsPaths.empty()) {
      ti.spawn(QueueJob{ this, [this, &system, ti, completionFn, result](QueueJobContext* context) mutable {
            if (!processDiscoveredDependencies(system, ti, context)) {
              // If we were unable to process the dependencies output, report a
              // failure.
              if (completionFn.hasValue())
                completionFn.getValue()(ProcessStatus::Failed);
              return;
            }
            if (completionFn.hasValue())
              completionFn.getValue()(result);
          }}, QueueJobPriority::High);
      return;
    }

    if (completionFn.hasValue())
      completionFn.getValue()(result);
  };
      
  // Delegate to the handler, if present.
  if (handler) {
    // FIXME: We should consider making this interface capable of feeding
    // back the dependencies directly.
    //
    // FIXME: This needs to honor certain properties of the execution queue
    // (like indicating when the work starts and stops, and communicating with
    // the execution queue delegate controlling how output is handled). It could
    // be the case that this should actually be delegating this work to run on
    // the execution queue, and the queue handles the handoff.
    handler->execute(
        handlerState.get(), this, ti, context, commandCompletionFn);
    return;
  }

  bool connectToConsole = false;

  // Compose the environment now, at the point it is actually needed. Only the
  // commands that run pay for this.
  SmallVector<std::pair<StringRef, StringRef>, 16> effectiveEnv;
  getEffectiveEnv(effectiveEnv);

  // Execute the command.
  ti.spawn(
      context, args, effectiveEnv,
      {canSafelyInterrupt, connectToConsole, workingDirectory, inheritEnv, controlEnabled},
      /*completionFn=*/{commandCompletionFn});
}
