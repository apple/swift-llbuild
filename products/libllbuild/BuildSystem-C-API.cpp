//===-- BuildSystem-C-API.cpp ---------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2015 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// Include the public API.
#include <llbuild/llbuild.h>

#include "llbuild/Basic/Defer.h"
#include "llbuild/Basic/FileSystem.h"
#include "llbuild/BuildSystem/BuildDescriptionBuilder.h"
#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/Command.h"
#include "llbuild/BuildSystem/BuildKey.h"
#include "llbuild/BuildSystem/BuildSystemFrontend.h"
#include "llbuild/BuildSystem/BuildValue.h"
#include "llbuild/BuildSystem/ExternalCommand.h"
#include "llbuild/BuildSystem/Tool.h"
#include "llbuild/Core/BuildEngine.h"
#include "llbuild/Core/DependencyInfoParser.h"
#include "llbuild/Core/MakefileDepsParser.h"

#include "BuildKey-C-API-Private.h"
#include "BuildValue-C-API-Private.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Path.h"

#include <atomic>
#include <cassert>
#include <future>
#include <memory>
#include <utility>

using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::buildsystem;

static Optional<QualityOfService> getQoSFromLLBQoS(llb_quality_of_service_t level);

/* Build Engine API */

namespace {
class CAPIFileSystem : public basic::FileSystem {
  llb_buildsystem_delegate_t cAPIDelegate;
  std::unique_ptr<basic::FileSystem> localFileSystem;
  
public:
  CAPIFileSystem(llb_buildsystem_delegate_t delegate)
      : cAPIDelegate(delegate),
        localFileSystem(basic::createLocalFileSystem()) { }

  virtual bool
  createDirectory(const std::string& path) override {
    if (!cAPIDelegate.fs_create_directory) {
      return localFileSystem->createDirectory(path);
    }

    return cAPIDelegate.fs_create_directory(cAPIDelegate.context, path.c_str());
  }
  
  virtual std::unique_ptr<llvm::MemoryBuffer>
  getFileContents(const std::string& path) override {
    if (!cAPIDelegate.fs_get_file_contents) {
      return localFileSystem->getFileContents(path);
    }
    
    llb_data_t data;
    if (!cAPIDelegate.fs_get_file_contents(cAPIDelegate.context, path.c_str(),
                                           &data)) {
      return nullptr;
    }

    // Create a new memory buffer to copy the data to.
    //
    // FIXME: This is an unfortunate amount of copying.
    llvm::StringRef fileContents = llvm::StringRef((const char*)data.data, data.length);
    auto result = llvm::MemoryBuffer::getMemBufferCopy(fileContents, path);

    // Release the client memory.
    //
    // FIXME: This is gross, come up with a general purpose solution.
    free((char*)data.data);

    return result;
  }

  virtual bool remove(const std::string& path) override {
    if (!cAPIDelegate.fs_remove) {
      return localFileSystem->remove(path);
    }

    return cAPIDelegate.fs_remove(cAPIDelegate.context, path.c_str());
  }

  virtual basic::FileChecksum getFileChecksum(const std::string& path) override {
    return localFileSystem->getFileChecksum(path);
  }

  virtual basic::FileInfo getFileInfo(const std::string& path) override {
    if (!cAPIDelegate.fs_get_file_info) {
      return localFileSystem->getFileInfo(path);
    }

    llb_fs_file_info_t file_info;
    cAPIDelegate.fs_get_file_info(cAPIDelegate.context, path.c_str(),
                                  &file_info);

    basic::FileInfo result{};
    result.device = file_info.device;
    result.inode = file_info.inode;
    result.mode = file_info.mode;
    result.size = file_info.size;
    result.modTime.seconds = file_info.mod_time.seconds;;
    result.modTime.nanoseconds = file_info.mod_time.nanoseconds;
    return result;
  }

  virtual basic::FileInfo getLinkInfo(const std::string& path) override {
    if (!cAPIDelegate.fs_get_link_info && !cAPIDelegate.fs_get_file_info) {
      return localFileSystem->getLinkInfo(path);
    }

    llb_fs_file_info_t file_info;
    if (cAPIDelegate.fs_get_link_info) {
      cAPIDelegate.fs_get_link_info(cAPIDelegate.context, path.c_str(),
                                    &file_info);
    } else {
      cAPIDelegate.fs_get_file_info(cAPIDelegate.context, path.c_str(),
                                    &file_info);
    }

    basic::FileInfo result{};
    result.device = file_info.device;
    result.inode = file_info.inode;
    result.mode = file_info.mode;
    result.size = file_info.size;
    result.modTime.seconds = file_info.mod_time.seconds;;
    result.modTime.nanoseconds = file_info.mod_time.nanoseconds;
    return result;
  }

  virtual bool createSymlink(const std::string& src, const std::string& target) override {
    if (!cAPIDelegate.fs_create_symlink) {
      return localFileSystem->createSymlink(src, target);
    }

    return cAPIDelegate.fs_create_symlink(cAPIDelegate.context, src.c_str(), target.c_str());
  }
};
  
llb_buildsystem_command_result_t get_command_result(ProcessStatus commandResult) {
  switch (commandResult) {
    case ProcessStatus::Succeeded:
      return llb_buildsystem_command_result_succeeded;
    case ProcessStatus::Cancelled:
      return llb_buildsystem_command_result_cancelled;
    case ProcessStatus::Failed:
      return llb_buildsystem_command_result_failed;
    case ProcessStatus::Skipped:
      return llb_buildsystem_command_result_skipped;
    default:
      assert(0 && "unknown command result");
      break;
  }
  return llb_buildsystem_command_result_failed;
}

class CAPIBuildSystemFrontendDelegate : public BuildSystemFrontendDelegate {
  llb_buildsystem_delegate_t cAPIDelegate;

public:
  CAPIBuildSystemFrontendDelegate(llvm::SourceMgr& sourceMgr,
                                  BuildSystemInvocation& invocation,
                                  llb_buildsystem_delegate_t delegate)
      : BuildSystemFrontendDelegate(sourceMgr, "basic", 0),
        cAPIDelegate(delegate) { }

  virtual std::unique_ptr<Tool> lookupTool(StringRef name) override {
    if (!cAPIDelegate.lookup_tool) {
      return nullptr;
    }
    
    llb_data_t cName{ name.size(), (const uint8_t*) name.data() };
    auto tool = cAPIDelegate.lookup_tool(cAPIDelegate.context, &cName);
    if (!tool) {
      return nullptr;
    }

    return std::unique_ptr<Tool>((Tool*)tool);
  }

  virtual void hadCommandFailure() override {
    // Call the base implementation.
    BuildSystemFrontendDelegate::hadCommandFailure();

    // Report the command failure, if the client provided an appropriate
    // callback.
    if (cAPIDelegate.had_command_failure) {
      cAPIDelegate.had_command_failure(cAPIDelegate.context);
    } else {
      // Otherwise, the default behavior is to immediately cancel.
      cancel();
    }
  }

  static llb_buildsystem_command_status_kind_t
  convertStatusKind(BuildSystemDelegate::CommandStatusKind kind) {
    switch (kind) {
    case BuildSystemDelegate::CommandStatusKind::IsScanning:
      return llb_buildsystem_command_status_kind_is_scanning;
    case BuildSystemDelegate::CommandStatusKind::IsUpToDate:
      return llb_buildsystem_command_status_kind_is_up_to_date;
    case BuildSystemDelegate::CommandStatusKind::IsComplete:
      return llb_buildsystem_command_status_kind_is_complete;
    }
    assert(0 && "unknown status kind");
    return llb_buildsystem_command_status_kind_is_scanning;
  }
  
  virtual void
  commandStatusChanged(Command* command,
                       BuildSystemDelegate::CommandStatusKind kind) override {
    if (cAPIDelegate.command_status_changed) {
      cAPIDelegate.command_status_changed(
          cAPIDelegate.context, (llb_buildsystem_command_t*) command,
          convertStatusKind(kind));
    }
  }

  virtual void commandPreparing(Command* command) override {
    if (cAPIDelegate.command_preparing) {
      cAPIDelegate.command_preparing(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command);
    }
  }

  virtual bool shouldCommandStart(Command * command) override {
    if (cAPIDelegate.should_command_start) {
      return cAPIDelegate.should_command_start(
          cAPIDelegate.context, (llb_buildsystem_command_t*) command);
    } else {
      return true;
    }
  }

  virtual void commandStarted(Command* command) override {
    if (cAPIDelegate.command_started) {
      cAPIDelegate.command_started(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command);
    }
  }

  virtual void commandFinished(Command* command, ProcessStatus commandResult) override {
    if (cAPIDelegate.command_finished) {
      cAPIDelegate.command_finished(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command,
          get_command_result(commandResult));
    }
  }

  virtual void commandFoundDiscoveredDependency(Command* command, StringRef path, DiscoveredDependencyKind kind) override {
    if (cAPIDelegate.command_found_discovered_dependency) {
      cAPIDelegate.command_found_discovered_dependency(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command,
          path.str().c_str(),
          (llb_buildsystem_discovered_dependency_kind_t) kind);
    }
  }

  virtual void commandHadError(Command* command, StringRef message) override {
    if (cAPIDelegate.command_had_error) {
      llb_data_t cMessage { message.size(), (const uint8_t*) message.data() };
      cAPIDelegate.command_had_error(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command,
          &cMessage);
    }
  }

  virtual void commandHadNote(Command* command, StringRef message) override {
    if (cAPIDelegate.command_had_note) {
      llb_data_t cMessage { message.size(), (const uint8_t*) message.data() };
      cAPIDelegate.command_had_note(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command,
          &cMessage);
    }
  }

  virtual void commandHadWarning(Command* command, StringRef message) override {
    if (cAPIDelegate.command_had_warning) {
      llb_data_t cMessage { message.size(), (const uint8_t*) message.data() };
      cAPIDelegate.command_had_warning(
         cAPIDelegate.context,
         (llb_buildsystem_command_t*) command,
         &cMessage);
    }
  }

  virtual void commandCannotBuildOutputDueToMissingInputs(Command* command,
               Node* outputNode, ArrayRef<BuildKey> inputKeys) override {
    if (cAPIDelegate.command_cannot_build_output_due_to_missing_inputs) {
      CAPIBuildKey *output = nullptr;
      if (outputNode) {
        auto str = outputNode->getName().str();
        output = new CAPIBuildKey(BuildKey::makeNode(str));
      }

      CAPIBuildKeysVector inputs(inputKeys);

      cAPIDelegate.command_cannot_build_output_due_to_missing_inputs(
        cAPIDelegate.context,
        (llb_buildsystem_command_t*) command,
        (llb_build_key_t **)&output,
        inputs.data(),
        inputs.count()
      );

      llb_build_key_destroy((llb_build_key_t *)output);
    }
  }

  class CAPIBuildKeysVector {
  private:
    std::vector<CAPIBuildKey *> keys;

  public:
    CAPIBuildKeysVector(const ArrayRef<BuildKey> inputKeys) : keys(inputKeys.size()) {
      int idx = 0;
      for (auto inputKey : inputKeys) {
        keys[idx++] = new CAPIBuildKey(inputKey);
      }
    }

    ~CAPIBuildKeysVector() {
      for (auto key : keys) {
        llb_build_key_destroy((llb_build_key_t *)key);
      }
    }

    llb_build_key_t** data() { return (llb_build_key_t **)keys.data(); }
    uint64_t count() { return keys.size(); }
  };

  virtual Command* chooseCommandFromMultipleProducers(Node* outputNode,
                   std::vector<Command*> commands) override {
    if (cAPIDelegate.choose_command_from_multiple_producers) {
      auto str = outputNode->getName().str();
      auto output = (llb_build_key_t *)new CAPIBuildKey(BuildKey::makeNode(str));

      llb_buildsystem_command_t* command = cAPIDelegate.choose_command_from_multiple_producers(
        cAPIDelegate.context,
        &output,
        (llb_buildsystem_command_t**)commands.data(),
        commands.size()
      );

      llb_build_key_destroy(output);

      return (Command*)command;
    } else {
      return nullptr;
    }
  }

  virtual void cannotBuildNodeDueToMultipleProducers(Node* outputNode,
               std::vector<Command*> commands) override {
    if (cAPIDelegate.cannot_build_node_due_to_multiple_producers) {
      auto str = outputNode->getName().str();
      auto output = (llb_build_key_t *)new CAPIBuildKey(BuildKey::makeNode(str));

      cAPIDelegate.cannot_build_node_due_to_multiple_producers(
        cAPIDelegate.context,
        &output,
        (llb_buildsystem_command_t**)commands.data(),
        commands.size()
      );
      
      llb_build_key_destroy(output);
    }
  }

  virtual void commandProcessStarted(Command* command,
                                     ProcessHandle handle) override {
    if (cAPIDelegate.command_process_started) {
      cAPIDelegate.command_process_started(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command,
          (llb_buildsystem_process_t*) handle.id);
    }
  }

  virtual void commandProcessHadError(Command* command, ProcessHandle handle,
                                      const Twine& message) override {
    if (cAPIDelegate.command_process_had_error) {
      SmallString<256> data;
      message.toVector(data);
      llb_data_t cData{ data.size(), (const uint8_t*) data.data() };
      cAPIDelegate.command_process_had_error(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command,
          (llb_buildsystem_process_t*) handle.id,
          &cData);
    }
  }

  virtual void commandProcessHadOutput(Command* command, ProcessHandle handle,
                                       StringRef data) override {
    if (cAPIDelegate.command_process_had_output) {
      llb_data_t cData{ data.size(), (const uint8_t*) data.data() };
      cAPIDelegate.command_process_had_output(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command,
          (llb_buildsystem_process_t*) handle.id,
          &cData);
    }
  }
  
  virtual void commandProcessFinished(Command* command, ProcessHandle handle,
                                      const ProcessResult& commandResult) override {
    if (cAPIDelegate.command_process_finished) {
      llb_buildsystem_command_extended_result_t result;
      result.result = get_command_result(commandResult.status);
      result.exit_status = commandResult.exitCode;
      result.pid = commandResult.pid;
      result.utime = commandResult.utime;
      result.stime = commandResult.stime;
      result.maxrss = commandResult.maxrss;

      cAPIDelegate.command_process_finished(
          cAPIDelegate.context,
          (llb_buildsystem_command_t*) command,
          (llb_buildsystem_process_t*) handle.id,
          &result);
    }
  }

  /// Request cancellation of any current build.
  void cancel() override {
    BuildSystemFrontendDelegate::cancel();
  }

  class CAPIRulesVector {
  private:
    std::vector<CAPIBuildKey *> rules;

  public:
    CAPIRulesVector(const std::vector<core::Rule*>& items) : rules(items.size()) {
      int idx = 0;

      for (std::vector<core::Rule*>::const_iterator it = items.begin(); it != items.end(); ++it) {
        core::Rule* rule = *it;
        auto key = BuildKey::fromData(rule->key);
        rules[idx++] = new CAPIBuildKey(key);
      }
    }

    ~CAPIRulesVector() {
      for (auto rule : rules) {
        llb_build_key_destroy((llb_build_key_t *)rule);
      }
    }

    llb_build_key_t** data() { return (llb_build_key_t** )&rules[0]; }
    uint64_t count() { return rules.size(); }
  };

  static llb_rule_run_reason_t
  convertRunReason(core::Rule::RunReason reason) {
    switch (reason) {
      case core::Rule::RunReason::NeverBuilt:
        return llb_rule_run_reason_never_built;
      case core::Rule::RunReason::SignatureChanged:
        return llb_rule_run_reason_signature_changed;
      case core::Rule::RunReason::InvalidValue:
        return llb_rule_run_reason_invalid_value;
      case core::Rule::RunReason::InputRebuilt:
        return llb_rule_run_reason_input_rebuilt;
      case core::Rule::RunReason::Forced:
        return llb_rule_run_reason_forced;
    }
    assert(0 && "unknown reason");
    return llb_rule_run_reason_invalid_value;
  }

  virtual void determinedRuleNeedsToRun(core::Rule* ruleNeedingToRun, core::Rule::RunReason reason, core::Rule* inputRule) override {
    if (cAPIDelegate.determined_rule_needs_to_run) {
      auto key = BuildKey::fromData(ruleNeedingToRun->key);
      auto needsToRun = (llb_build_key_t *)new CAPIBuildKey(key);
      llb_build_key_t * input;
      if (inputRule) {
        auto inputKey = BuildKey::fromData(inputRule->key);
        input = (llb_build_key_t *)new CAPIBuildKey(inputKey);
      } else {
        input = nullptr;
      }
      cAPIDelegate.determined_rule_needs_to_run(cAPIDelegate.context, needsToRun, convertRunReason(reason), input);
      llb_build_key_destroy(needsToRun);
      if (input) {
        llb_build_key_destroy(input);
      }
    }
  }

  virtual void cycleDetected(const std::vector<core::Rule*>& items) override {
    CAPIRulesVector rules(items);
    cAPIDelegate.cycle_detected(cAPIDelegate.context, rules.data(), rules.count());
  }

  static llb_cycle_action_t
  convertCycleAction(core::Rule::CycleAction action) {
    switch (action) {
      case core::Rule::CycleAction::ForceBuild:
        return llb_cycle_action_force_build;
      case core::Rule::CycleAction::SupplyPriorValue:
        return llb_cycle_action_supply_prior_value;
    }
    assert(0 && "unknown cycle action");
    return llb_cycle_action_force_build;
  }

  virtual bool shouldResolveCycle(const std::vector<core::Rule*>& items,
                                  core::Rule* candidateRule,
                                  core::Rule::CycleAction action) override {
    if (!cAPIDelegate.should_resolve_cycle)
      return false;

    CAPIRulesVector rules(items);
    auto key = BuildKey::fromData(candidateRule->key);
    auto candidate = (llb_build_key_t *)new CAPIBuildKey(key);

    uint8_t result = cAPIDelegate.should_resolve_cycle(cAPIDelegate.context,
                                                       rules.data(),
                                                       rules.count(),
                                                       candidate,
                                                       convertCycleAction(action));

    llb_build_key_destroy(candidate);

    return (result);
  }

  virtual void error(StringRef filename, const Token& at, const Twine& message) override {
    if (cAPIDelegate.handle_diagnostic) {
      cAPIDelegate.handle_diagnostic(cAPIDelegate.context,
                                     llb_buildsystem_diagnostic_kind_error,
                                     filename.str().c_str(),
                                     -1,
                                     -1,
                                     message.str().c_str());
    } else {
        BuildSystemFrontendDelegate::error(filename, at, message);
    }
  }
};

/// The FFI-side half of an in-memory build description.
///
/// `BuildDescriptionBuilder` deals in `std::unique_ptr<Command>`, but a C client
/// can only hold a raw handle between `begin_command` and `finish_command`. This
/// owns the commands over that window, keyed by the handle it vended, and hands
/// them to the builder on finish.
class CAPIDescriptionBuilder {
  BuildDescriptionBuilder& builder;
  llvm::DenseMap<Command*, std::unique_ptr<Command>> pendingCommands;

public:
  CAPIDescriptionBuilder(BuildDescriptionBuilder& builder)
      : builder(builder) {}

  BuildDescriptionBuilder& getBuilder() { return builder; }

  Command* beginCommand(StringRef name, StringRef toolName) {
    auto command = builder.createCommand(name, toolName);
    if (!command)
      return nullptr;

    auto result = command.get();
    pendingCommands[result] = std::move(command);
    return result;
  }

  void finishCommand(StringRef name, Command* command) {
    auto it = pendingCommands.find(command);
    if (it == pendingCommands.end()) {
      // Not a live handle from this builder; nothing safe to do.
      assert(0 && "finishCommand on an unknown command handle");
      return;
    }

    auto owned = std::move(it->second);
    pendingCommands.erase(it);
    builder.addCommand(name, std::move(owned));
  }
};

class CAPIBuildSystem {
  llb_buildsystem_delegate_t cAPIDelegate;
  
  BuildSystemInvocation invocation;
  
  llvm::SourceMgr sourceMgr;

  std::unique_ptr<CAPIBuildSystemFrontendDelegate> frontendDelegate;
  std::unique_ptr<BuildSystemFrontend> frontend;

  void handleDiagnostic(const llvm::SMDiagnostic& diagnostic) {
    llb_buildsystem_diagnostic_kind_t kind = llb_buildsystem_diagnostic_kind_note;
    switch (diagnostic.getKind()) {
    case llvm::SourceMgr::DK_Error:
      kind = llb_buildsystem_diagnostic_kind_error;
      break;
    case llvm::SourceMgr::DK_Warning:
      kind = llb_buildsystem_diagnostic_kind_warning;
      break;
    case llvm::SourceMgr::DK_Note:
    case llvm::SourceMgr::DK_Remark:
      kind = llb_buildsystem_diagnostic_kind_note;
      break;
    }

    // FIXME: We don't currently expose the caret diagnostic information, or
    // fixits. llbuild does currently make use of the caret diagnostics for
    // reporting problems in build manifest files...
    cAPIDelegate.handle_diagnostic(
        cAPIDelegate.context, kind,
        diagnostic.getFilename().str().c_str(),
        diagnostic.getLineNo(), diagnostic.getColumnNo(),
        diagnostic.getMessage().str().c_str());
  }
  
public:
  CAPIBuildSystem(llb_buildsystem_delegate_t delegate,
                  llb_buildsystem_invocation_t cAPIInvocation,
                  StringRef descriptionOriginName = "",
                  void* descriptionContext = nullptr,
                  bool (*populateDescription)(
                      void*, llb_buildsystem_description_builder_t*) = nullptr)
    : cAPIDelegate(delegate)
  {
    // Convert the invocation.
    invocation.buildFilePath =
      cAPIInvocation.buildFilePath ? cAPIInvocation.buildFilePath : "";
    invocation.dbPath = cAPIInvocation.dbPath ? cAPIInvocation.dbPath : "";
    invocation.traceFilePath = (
        cAPIInvocation.traceFilePath ? cAPIInvocation.traceFilePath : "");
    invocation.environment = cAPIInvocation.environment;
    invocation.useSerialBuild = cAPIInvocation.useSerialBuild;
    invocation.showVerboseStatus = cAPIInvocation.showVerboseStatus;
    invocation.schedulerLanes = cAPIInvocation.schedulerLanes;
    invocation.qos = getQoSFromLLBQoS(cAPIInvocation.qos);

    // Register a custom diagnostic handler with the source manager.
    sourceMgr.setDiagHandler([](const llvm::SMDiagnostic& diagnostic,
                                void* context) {
        auto system = (CAPIBuildSystem*) context;
        system->handleDiagnostic(diagnostic);
      }, this);
    
    // Allocate the frontend delegate.
    frontendDelegate.reset(
        // FIXME: Need to get the client name and schema version from
        // parameters.
        new CAPIBuildSystemFrontendDelegate(sourceMgr, invocation, delegate));

    // Allocate the file system
    std::unique_ptr<basic::FileSystem> fileSystem(new CAPIFileSystem(delegate));

    // Allocate the actual frontend, over whichever description source the
    // client asked for.
    if (populateDescription) {
      frontend.reset(new BuildSystemFrontend(
          *frontendDelegate, invocation, std::move(fileSystem),
          [descriptionContext, populateDescription](
              BuildDescriptionBuilder& builder) -> bool {
            CAPIDescriptionBuilder capiBuilder(builder);
            return populateDescription(
                descriptionContext,
                (llb_buildsystem_description_builder_t*) &capiBuilder);
          },
          descriptionOriginName));
    } else {
      frontend.reset(new BuildSystemFrontend(*frontendDelegate, invocation,
                                             std::move(fileSystem)));
    }
  }

  BuildSystemFrontend& getFrontend() {
    return *frontend;
  }

  bool initialize() {
    return getFrontend().initialize();
  }

  bool build(llvm::StringRef key) {
    return getFrontend().build(key);
  }

  bool buildNode(llvm::StringRef key) {
    return getFrontend().buildNode(key);
  }

  void cancel() {
    frontendDelegate->cancel();
  }
};

class CAPITool : public Tool {
  llb_buildsystem_tool_delegate_t cAPIDelegate;
  
public:
  CAPITool(StringRef name, llb_buildsystem_tool_delegate_t delegate)
      : Tool(name), cAPIDelegate(delegate) {}

  virtual bool configureAttribute(const ConfigureContext& context,
                                  StringRef name,
                                  StringRef value) override {
    // FIXME: Support custom attributes in client tools.
    return false;
  }
  
  virtual bool configureAttribute(const ConfigureContext& context,
                                  StringRef name,
                                  ArrayRef<StringRef> values) override {
    // FIXME: Support custom attributes in client tools.
    return false;
  }

  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
    // FIXME: Support custom attributes in client tools.
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(StringRef name) override {
    llb_data_t cName{ name.size(), (const uint8_t*) name.data() };
    return std::unique_ptr<Command>(
        (Command*) cAPIDelegate.create_command(cAPIDelegate.context, &cName));
  }

  virtual std::unique_ptr<Command>
  createCustomCommand(const BuildKey& key) override {
    auto key_p = (llb_build_key_t *)new CAPIBuildKey(key);
    llbuild_defer {
      llb_build_key_destroy(key_p);
    };
    return std::unique_ptr<Command>(
        (Command*) cAPIDelegate.create_custom_command(cAPIDelegate.context, key_p));
  }
  
  ~CAPITool() {
    if (cAPIDelegate.destroy_context) {
      cAPIDelegate.destroy_context(cAPIDelegate.context);
    }
  }
};

class CAPIExternalCommand : public ExternalCommand {
  // FIXME: This is incredibly wasteful to copy everywhere. Rephrase things so
  // that the delegates are const and we just carry the context pointer around.
  llb_buildsystem_external_command_delegate_t cAPIDelegate;
  
  /// The paths to the dependency output files, if used.
  SmallVector<std::string, 1> depsPaths{};
  
  /// Names of output nodes of this command.
  SmallVector<std::string, 1> outputNodeNames{};

  /// The working directory used to resolve relative paths in dependency files.
  std::string workingDirectory;
  
  /// Format of the dependencies in `depsPaths`.
  /// We default to `dependencyinfo` for legacy behavior since not all tasks specify the format.
  llb_buildsystem_dependency_data_format_t depsFormat =
          llb_buildsystem_dependency_data_format_dependencyinfo;

  bool processDependencyInfoDiscoveredDependencies(BuildSystem& system,
                                                   core::TaskInterface ti,
                                                   QueueJobContext* context,
                                                   std::string depsPath) {
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
      system.getDelegate().commandHadError(this, "unable to open dependencies file (" + depsPath + ")");
      return false;
    }
    
    // Parse the output.
    //
    // We just ignore the rule, and add any dependency that we encounter in the
    // file.
    struct DepsActions : public core::DependencyInfoParser::ParseActions {
      BuildSystem& system;
      core::TaskInterface ti;
      CAPIExternalCommand* command;
      StringRef depsPath;
      unsigned numErrors{0};
      
      DepsActions(BuildSystem& system,
                  core::TaskInterface ti,
                  CAPIExternalCommand* command, StringRef depsPath)
      : system(system), ti(ti), command(command), depsPath(depsPath) {}
      
      virtual void error(const char* message, uint64_t position) override {
        system.getDelegate().commandHadError(command, "error reading dependency file '" + depsPath.str() + "': " + std::string(message));
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
  
  bool processMakefileDiscoveredDependencies(BuildSystem& system,
                                             core::TaskInterface ti,
                                             QueueJobContext* context,
                                             std::string depsPath,
                                             bool ignoreSubsequentOutputs) {
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
      system.getDelegate().commandHadError(this, "unable to open dependencies file (" + depsPath + ")");
      return false;
    }
    
    // Parse the output.
    //
    // We just ignore the rule, and add any dependency that we encounter in the
    // file.
    struct DepsActions : public core::MakefileDepsParser::ParseActions {
      BuildSystem& system;
      core::TaskInterface ti;
      CAPIExternalCommand* command;
      StringRef depsPath;
      unsigned numErrors{0};
      
      DepsActions(BuildSystem& system,
                  core::TaskInterface ti,
                  CAPIExternalCommand* command, StringRef depsPath)
      : system(system), ti(ti), command(command), depsPath(depsPath) {}
      
      virtual void error(StringRef message, uint64_t position) override {
        system.getDelegate().commandHadError(command, "error reading dependency file '" + depsPath.str() + "': " + std::string(message));
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
  
  bool configureAttributeNoContext(StringRef name, StringRef value) {
    if (name == "deps") {
      depsPaths.clear();
      depsPaths.emplace_back(value);
    } else if (name == "deps-style") {
      if (value == "unused") {
        depsFormat = llb_buildsystem_dependency_data_format_unused;
      } else if (value == "makefile") {
        depsFormat = llb_buildsystem_dependency_data_format_makefile;
      } else if (value == "dependency-info") {
        depsFormat = llb_buildsystem_dependency_data_format_dependencyinfo;
      } else if (value == "makefile-ignoring-subsequent-outputs") {
        depsFormat = llb_buildsystem_dependency_data_format_makefile_ignoring_subsequent_outputs;
      } else {
        return false;
      }
    } else if (name == "repair-via-ownership-analysis") {
      if (value == "true") {
        repairViaOwnershipAnalysis = true;
        return true;
      } else if (value == "false") {
        repairViaOwnershipAnalysis = false;
        return true;
      } else {
        return false;
      }
    } else if (name == "working-directory") {
      // Ensure the working directory is absolute. This will make sure any
      // relative directories are interpreted as relative to the CWD at the time
      // the rule is defined.
      SmallString<PATH_MAX> wd = value;
      llvm::sys::fs::make_absolute(wd);
      workingDirectory = StringRef(wd);
    } else if (name == "outputs") {
      outputNodeNames.clear();
      outputNodeNames.emplace_back(value);
    } else {
      return false;
    }
    return true;
  }
  
  bool configureAttributeNoContext(StringRef name,
                                  ArrayRef<StringRef> values) {
    if (name == "deps") {
      depsPaths.clear();
      depsPaths.insert(depsPaths.begin(), values.begin(), values.end());
    } else if (name == "outputs") {
      outputNodeNames.clear();
      outputNodeNames.insert(outputNodeNames.begin(), values.begin(), values.end());
    } else {
      return false;
    }
    return true;
  }
  bool configureAttributeNoContext(StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) {
    return false;
  }

  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  StringRef value) override {
    if (configureAttributeNoContext(name, value)) {
      return true;
    } else {
      return ExternalCommand::configureAttribute(ctx, name, value);
    }
  }
  
  virtual bool configureAttribute(const ConfigureContext& ctx, StringRef name,
                                  ArrayRef<StringRef> values) override {
    if (configureAttributeNoContext(name, values)) {
      return true;
    } else {
      return ExternalCommand::configureAttribute(ctx, name, values);
    }
  }
  virtual bool configureAttribute(
      const ConfigureContext& ctx, StringRef name,
      ArrayRef<std::pair<StringRef, StringRef>> values) override {
        if (configureAttributeNoContext(name, values)) {
          return true;
        } else {
          return ExternalCommand::configureAttribute(ctx, name, values);
        }
  }

  /// Resolve output node names to actual BuildNode pointers if not already done.
  void resolveOutputs(BuildSystem& system) {
    if (outputs.empty() && !outputNodeNames.empty()) {
      outputs.reserve(outputNodeNames.size());
      for (const auto& name : outputNodeNames) {
        outputs.push_back(system.lookupNode(name));
      }
    }
  }

  virtual void startExternalCommand(BuildSystem& system,
                                    core::TaskInterface ti) override {
    resolveOutputs(system);
    cAPIDelegate.start(cAPIDelegate.context,
                       (llb_buildsystem_command_t*)this,
                       (llb_buildsystem_interface_t*)&system,
                       *reinterpret_cast<llb_task_interface_t*>(&ti));
  }

  void provideValueExternalCommand(BuildSystem& system,
                                   core::TaskInterface ti,
                                   uintptr_t inputID,
                                   const BuildValue& value) override {
    auto value_p = (llb_build_value *)new CAPIBuildValue(BuildValue(value));
    cAPIDelegate.provide_value(cAPIDelegate.context,
                               (llb_buildsystem_command_t*)this,
                               (llb_buildsystem_interface_t*)&system,
                               *reinterpret_cast<llb_task_interface_t*>(&ti),
                               value_p,
                               inputID);
  }

  // Temporary holder for the current build value. Only expected to be valid
  // for the duration of a call to the completion function passed to
  // executeExternalCommand().
  CAPIBuildValue* currentBuildValue = nullptr;

  std::atomic<bool> detachedCommandFinished{false};
  std::unique_ptr<core::CancellationDelegate> cancellationDelegate = nullptr;

  bool isDetached() const override {
    return cAPIDelegate.execute_command_detached != nullptr;
  }

  static ProcessStatus getProcessStatusFromLLBResult(llb_buildsystem_command_result_t result) {
    switch (result) {
      case llb_buildsystem_command_result_succeeded:
        return ProcessStatus::Succeeded;
      case llb_buildsystem_command_result_failed:
        return ProcessStatus::Failed;
      case llb_buildsystem_command_result_cancelled:
        return ProcessStatus::Cancelled;
      case llb_buildsystem_command_result_skipped:
        return ProcessStatus::Skipped;
    }
  }

  virtual void executeExternalCommand(BuildSystem& system,
                                      core::TaskInterface ti,
                                      QueueJobContext* job_context,
                                      llvm::Optional<ProcessCompletionFn> completionFn) override {
    auto doneFn = [this, &system, ti, job_context, completionFn](ProcessStatus result) mutable {
      if (result != ProcessStatus::Succeeded) {
        // If the command did not succeed, there is no need to gather dependencies.
        if (completionFn.hasValue())
          completionFn.getValue()(result);
        return;
      }

      // Otherwise, collect the discovered dependencies, if used.
      bool dependencyParsingResult = false;
      if (!depsPaths.empty()) {
        for (const auto& depsPath: depsPaths) {
          switch (depsFormat) {
            case llb_buildsystem_dependency_data_format_unused:
              ti.delegate()->error("No dependency format specified for discovered dependency files.");
              dependencyParsingResult = false;
              break;
            case llb_buildsystem_dependency_data_format_makefile:
              dependencyParsingResult = processMakefileDiscoveredDependencies(system, ti, job_context, depsPath, false);
              break;
            case llb_buildsystem_dependency_data_format_dependencyinfo:
              dependencyParsingResult = processDependencyInfoDiscoveredDependencies(system, ti, job_context, depsPath);
              break;
            case llb_buildsystem_dependency_data_format_makefile_ignoring_subsequent_outputs:
              dependencyParsingResult = processMakefileDiscoveredDependencies(system, ti, job_context, depsPath, true);
              break;
          }
          
          if (!dependencyParsingResult) {
            // If we were unable to process the dependencies output, report a
            // failure.
            if (completionFn.hasValue())
              completionFn.getValue()(ProcessStatus::Failed);
            return;
          }
        }
      }

      if (completionFn.hasValue())
        completionFn.getValue()(result);
    };

    if (cAPIDelegate.execute_command_detached) {
      struct ResultCallbackContext {
        CAPIExternalCommand *thisCommand;
        BuildSystem *buildSystem;
        std::function<void(ProcessStatus result)> doneFn;

        static void callback(void* result_ctx,
                             llb_buildsystem_command_result_t result,
                             llb_build_value* rvalue) {
          ResultCallbackContext *ctx = static_cast<ResultCallbackContext*>(result_ctx);
          auto thisCommand = ctx->thisCommand;
          BuildSystem &system = *ctx->buildSystem;
          auto doneFn = std::move(ctx->doneFn);
          delete ctx;

          thisCommand->detachedCommandFinished = true;
          if (auto cancellationDelegate = thisCommand->cancellationDelegate.get()) {
            system.removeCancellationDelegate(cancellationDelegate);
            thisCommand->cancellationDelegate = nullptr;
          }

          thisCommand->currentBuildValue = reinterpret_cast<CAPIBuildValue*>(rvalue);
          llbuild_defer {
            delete thisCommand->currentBuildValue;
            thisCommand->currentBuildValue = nullptr;
          };
          doneFn(getProcessStatusFromLLBResult(result));
        }
      };
      auto *callbackCtx = new ResultCallbackContext{this, &system, std::move(doneFn)};
      cAPIDelegate.execute_command_detached(
        cAPIDelegate.context,
        (llb_buildsystem_command_t*)this,
        (llb_buildsystem_interface_t*)&system,
        *reinterpret_cast<llb_task_interface_t*>(&ti),
        (llb_buildsystem_queue_job_context_t*)job_context,
        callbackCtx, ResultCallbackContext::callback);

      if (cAPIDelegate.cancel_detached_command) {
        class CAPICancellationDelegate: public core::CancellationDelegate {
          CAPIExternalCommand *thisCommand;

        public:
          CAPICancellationDelegate(CAPIExternalCommand *thisCommand) : thisCommand(thisCommand) {}

          void buildCancelled() override {
            if (thisCommand->detachedCommandFinished)
              return;
            thisCommand->cAPIDelegate.cancel_detached_command(
              thisCommand->cAPIDelegate.context,
              (llb_buildsystem_command_t*)this);
          }
        };
        this->cancellationDelegate = std::make_unique<CAPICancellationDelegate>(this);
        system.addCancellationDelegate(this->cancellationDelegate.get());
      }
      return;
    }

    if (cAPIDelegate.execute_command_ex) {
      llb_build_value* rvalue = cAPIDelegate.execute_command_ex(
        cAPIDelegate.context,
        (llb_buildsystem_command_t*)this,
        (llb_buildsystem_interface_t*)&system,
        *reinterpret_cast<llb_task_interface_t*>(&ti),
        (llb_buildsystem_queue_job_context_t*)job_context);

      currentBuildValue = reinterpret_cast<CAPIBuildValue*>(rvalue);
      llbuild_defer {
        delete currentBuildValue;
        currentBuildValue = nullptr;
      };

      if (!currentBuildValue->getInternalBuildValue().isInvalid()) {
        doneFn(ProcessStatus::Succeeded);
        return;
      }

      // An invalid value is interpreted as an unsupported method call and falls
      // through to execute_command below.
    }

    assert(cAPIDelegate.execute_command != nullptr);
    llb_buildsystem_command_result_t result = cAPIDelegate.execute_command(
      cAPIDelegate.context,
      (llb_buildsystem_command_t*)this,
      (llb_buildsystem_interface_t*)&system,
      *reinterpret_cast<llb_task_interface_t*>(&ti),
      (llb_buildsystem_queue_job_context_t*)job_context);
    doneFn(getProcessStatusFromLLBResult(result));
  }

  BuildValue computeCommandResult(BuildSystem& system, core::TaskInterface ti) override {
    if (currentBuildValue)
      return BuildValue(currentBuildValue->getInternalBuildValue());

    return ExternalCommand::computeCommandResult(system, ti);
  }

  bool isResultValid(BuildSystem& system, const BuildValue& value) override {
    // Ensure outputs are resolved before checking validity. For dynamic tasks,
    // outputs are normally resolved in startExternalCommand (during execution),
    // but isResultValid is called earlier during the scan phase.
    resolveOutputs(system);

    if (cAPIDelegate.is_result_valid_with_fallback) {
      struct FallbackContext {
        BuildSystem *buildSystem;
        
        static bool handle(void* c_ctx,
                           llb_buildsystem_command_t* c_command,
                           llb_build_value* c_value) {
          auto* command = (CAPIExternalCommand*)c_command;
          auto* value = (CAPIBuildValue*)c_value;
          
          auto* ctx = static_cast<FallbackContext*>(c_ctx);
          BuildSystem& system = *ctx->buildSystem;
          delete ctx;
          
          return command->ExternalCommand::isResultValid(system, value->getInternalBuildValue());
        }
      };
      
      auto value_p = (llb_build_value *)new CAPIBuildValue(BuildValue(value));
      return cAPIDelegate.is_result_valid_with_fallback(
        cAPIDelegate.context,
        (llb_buildsystem_command_t*)this,
        value_p,
        new FallbackContext{&system},
        FallbackContext::handle
      );
    }
    
    if (cAPIDelegate.is_result_valid) {
      auto value_p = (llb_build_value *)new CAPIBuildValue(BuildValue(value));
      return cAPIDelegate.is_result_valid(
        cAPIDelegate.context,
        (llb_buildsystem_command_t*)this,
        value_p
      );
    }

    return ExternalCommand::isResultValid(system, value);
  }
  
public:
  CAPIExternalCommand(StringRef name,
                      llb_buildsystem_external_command_delegate_t delegate)
      : ExternalCommand(name), cAPIDelegate(delegate) {
        if (cAPIDelegate.configure) {
          auto single = [](llb_buildsystem_command_t* ctx, llb_data_t name, llb_data_t data) {
            ((CAPIExternalCommand *)ctx)->configureAttributeNoContext(StringRef((const char*)name.data, name.length), StringRef((const char*)data.data, data.length));
          };
          auto collection = [](llb_buildsystem_command_t* ctx, llb_data_t name, llb_data_t* datas, size_t size) {
            std::vector<StringRef> values;
            for (size_t i = 0; i < size; i++) {
              values.push_back(StringRef((const char*)datas[i].data, datas[i].length));
            }
            ((CAPIExternalCommand *)ctx)->configureAttributeNoContext(StringRef((const char*)name.data, name.length), ArrayRef<StringRef>(values));
          };
          auto map = [](llb_buildsystem_command_t* ctx, llb_data_t name, llb_data_t* keys, llb_data_t* values, size_t size) {
            std::vector<std::pair<StringRef, StringRef>> attributeValues;
            for (size_t i = 0; i < size; i++) {
              attributeValues.push_back(std::pair<StringRef, StringRef>(StringRef((const char*)keys[i].data, keys[i].length), StringRef((const char*)values[i].data, values[i].length)));
            }
            ((CAPIExternalCommand *)ctx)->configureAttributeNoContext(StringRef((const char*)name.data, name.length), ArrayRef<std::pair<StringRef, StringRef>>(attributeValues));
          };
          
          cAPIDelegate.configure(cAPIDelegate.context, (llb_buildsystem_command_t*)this, single, collection, map);
        }
      }


  virtual void getShortDescription(SmallVectorImpl<char> &result) const override {
    // FIXME: Provide client control.
    if (!getDescription().empty()) {
      llvm::raw_svector_ostream(result) << getDescription();
    } else {
      llvm::raw_svector_ostream(result) << getName();
    }
  }

  virtual void getVerboseDescription(SmallVectorImpl<char> &result) const override {
    // FIXME: Provide client control.
    if (!getDescription().empty()) {
      llvm::raw_svector_ostream(result) << getDescription();
    } else {
      llvm::raw_svector_ostream(result) << getName();
    }
  }

  virtual llbuild::basic::CommandSignature getSignature() const override {
    auto sig = ExternalCommand::getSignature();
    if (cAPIDelegate.get_signature) {
      llb_data_t data;
      cAPIDelegate.get_signature(cAPIDelegate.context, (llb_buildsystem_command_t*)this,
                                 &data);
      sig = sig.combine(StringRef((const char*)data.data, data.length));

      // Release the client memory.
      llb_data_destroy(&data);
    }
    return sig;
  }
  
  ~CAPIExternalCommand() {
    if (cAPIDelegate.destroy_context) {
      cAPIDelegate.destroy_context(cAPIDelegate.context);
    }
  }
};

}

const char* llb_buildsystem_diagnostic_kind_get_name(
    llb_buildsystem_diagnostic_kind_t kind) {
  switch (kind) {
  case llb_buildsystem_diagnostic_kind_note:
    return "note";
  case llb_buildsystem_diagnostic_kind_warning:
    return "warning";
  case llb_buildsystem_diagnostic_kind_error:
    return "error";
  default:
    return "<unknown>";
  }
}

llb_buildsystem_t* llb_buildsystem_create(
    llb_buildsystem_delegate_t delegate,
    llb_buildsystem_invocation_t invocation) {
  // Check that all required methods are provided.
  assert(delegate.handle_diagnostic);
  assert(delegate.command_started);
  assert(delegate.command_finished);
  assert(delegate.command_found_discovered_dependency);
  assert(delegate.command_process_started);
  assert(delegate.command_process_had_error);
  assert(delegate.command_process_had_output);
  assert(delegate.command_process_finished);
         
  return (llb_buildsystem_t*) new CAPIBuildSystem(delegate, invocation);
}

llb_buildsystem_t* llb_buildsystem_create_with_description(
    llb_buildsystem_delegate_t delegate,
    llb_buildsystem_invocation_t invocation,
    const llb_data_t* description_origin_name, void* context,
    bool (*populate_description)(
        void* context, llb_buildsystem_description_builder_t* builder)) {
  // Check that all required methods are provided.
  assert(delegate.handle_diagnostic);
  assert(delegate.command_started);
  assert(delegate.command_finished);
  assert(delegate.command_found_discovered_dependency);
  assert(delegate.command_process_started);
  assert(delegate.command_process_had_error);
  assert(delegate.command_process_had_output);
  assert(delegate.command_process_finished);
  assert(populate_description);

  return (llb_buildsystem_t*) new CAPIBuildSystem(
      delegate, invocation,
      StringRef((const char*)description_origin_name->data,
                description_origin_name->length),
      context, populate_description);
}

void llb_buildsystem_destroy(llb_buildsystem_t* system) {
  delete (CAPIBuildSystem*)system;
}

llb_buildsystem_tool_t*
llb_buildsystem_tool_create(const llb_data_t* name,
                            llb_buildsystem_tool_delegate_t delegate) {
  // Check that all required methods are provided.
  assert(delegate.create_command);
  return (llb_buildsystem_tool_t*) new CAPITool(
      StringRef((const char*)name->data, name->length), delegate);
}

bool llb_buildsystem_initialize(llb_buildsystem_t* system_p) {
  CAPIBuildSystem* system = (CAPIBuildSystem*) system_p;
  return system->initialize();
}

bool llb_buildsystem_build(llb_buildsystem_t* system_p, const llb_data_t* key) {
  CAPIBuildSystem* system = (CAPIBuildSystem*) system_p;
  return system->build(llvm::StringRef((const char*)key->data, key->length));
}

bool llb_buildsystem_build_node(llb_buildsystem_t* system_p, const llb_data_t* key) {
  CAPIBuildSystem* system = (CAPIBuildSystem*) system_p;
  return system->buildNode(llvm::StringRef((const char*)key->data, key->length));
}

void llb_buildsystem_cancel(llb_buildsystem_t* system_p) {
  CAPIBuildSystem* system = (CAPIBuildSystem*) system_p;
  system->cancel();
}

/* In-Memory Build Description API */

namespace {

StringRef toStringRef(const llb_data_t* data) {
  return StringRef((const char*)data->data, data->length);
}

/// The `llb_data_t` array C clients pass for list- and map-valued attributes,
/// viewed as `StringRef`s. Borrows the caller's buffer; valid only as long as it
/// is.
std::vector<StringRef> toStringRefs(const llb_data_t* data, uint64_t count) {
  std::vector<StringRef> result;
  result.reserve(count);
  for (uint64_t i = 0; i != count; ++i)
    result.push_back(toStringRef(&data[i]));
  return result;
}

std::vector<std::pair<StringRef, StringRef>> toStringRefPairs(
    const llb_data_t* keys, const llb_data_t* values, uint64_t count) {
  std::vector<std::pair<StringRef, StringRef>> result;
  result.reserve(count);
  for (uint64_t i = 0; i != count; ++i)
    result.emplace_back(toStringRef(&keys[i]), toStringRef(&values[i]));
  return result;
}

FileSystemMode getFileSystemModeFromLLBMode(
    llb_buildsystem_file_system_mode_t mode) {
  switch (mode) {
  case llb_buildsystem_file_system_mode_device_agnostic:
    return FileSystemMode::DeviceAgnostic;
  case llb_buildsystem_file_system_mode_checksum_only:
    return FileSystemMode::ChecksumOnly;
  case llb_buildsystem_file_system_mode_full:
    break;
  }
  return FileSystemMode::Full;
}

}

void llb_buildsystem_description_builder_set_file_system_mode(
    llb_buildsystem_description_builder_t* builder_p,
    llb_buildsystem_file_system_mode_t mode) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  builder->getBuilder().setFileSystemMode(
      getFileSystemModeFromLLBMode(mode));
}

void llb_buildsystem_description_builder_add_target(
    llb_buildsystem_description_builder_t* builder_p, const llb_data_t* name,
    const llb_data_t* nodes, uint64_t nodes_count) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  builder->getBuilder().addTarget(toStringRef(name),
                                  toStringRefs(nodes, nodes_count));
}

bool llb_buildsystem_description_builder_set_default_target(
    llb_buildsystem_description_builder_t* builder_p, const llb_data_t* name) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  return builder->getBuilder().setDefaultTarget(toStringRef(name));
}

bool llb_buildsystem_description_builder_set_node_attribute(
    llb_buildsystem_description_builder_t* builder_p,
    const llb_data_t* node_name, const llb_data_t* name,
    const llb_data_t* value) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  return builder->getBuilder().configureNodeAttribute(
      toStringRef(node_name), toStringRef(name), toStringRef(value));
}

bool llb_buildsystem_description_builder_set_node_attribute_list(
    llb_buildsystem_description_builder_t* builder_p,
    const llb_data_t* node_name, const llb_data_t* name,
    const llb_data_t* values, uint64_t values_count) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  return builder->getBuilder().configureNodeAttribute(
      toStringRef(node_name), toStringRef(name),
      toStringRefs(values, values_count));
}

bool llb_buildsystem_description_builder_set_node_attribute_map(
    llb_buildsystem_description_builder_t* builder_p,
    const llb_data_t* node_name, const llb_data_t* name,
    const llb_data_t* keys, const llb_data_t* values, uint64_t count) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  return builder->getBuilder().configureNodeAttribute(
      toStringRef(node_name), toStringRef(name),
      toStringRefPairs(keys, values, count));
}

bool llb_buildsystem_description_builder_add_environment_base(
    llb_buildsystem_description_builder_t* builder_p, const llb_data_t* name,
    const llb_data_t* keys, const llb_data_t* values, uint64_t count) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  return builder->getBuilder().addEnvironmentBase(
             toStringRef(name), toStringRefPairs(keys, values, count)) !=
         nullptr;
}

llb_buildsystem_command_t*
llb_buildsystem_description_builder_begin_command(
    llb_buildsystem_description_builder_t* builder_p, const llb_data_t* name,
    const llb_data_t* tool_name) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  return (llb_buildsystem_command_t*) builder->beginCommand(
      toStringRef(name), toStringRef(tool_name));
}

void llb_buildsystem_description_builder_set_command_inputs(
    llb_buildsystem_description_builder_t* builder_p,
    llb_buildsystem_command_t* command_p, const llb_data_t* nodes,
    uint64_t nodes_count) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  auto& b = builder->getBuilder();
  b.configureCommandInputs(*(Command*)command_p,
                           toStringRefs(nodes, nodes_count), b.getContext());
}

void llb_buildsystem_description_builder_set_command_outputs(
    llb_buildsystem_description_builder_t* builder_p,
    llb_buildsystem_command_t* command_p, const llb_data_t* nodes,
    uint64_t nodes_count) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  auto& b = builder->getBuilder();
  b.configureCommandOutputs(*(Command*)command_p,
                            toStringRefs(nodes, nodes_count), b.getContext());
}

void llb_buildsystem_description_builder_set_command_description(
    llb_buildsystem_description_builder_t* builder_p,
    llb_buildsystem_command_t* command_p, const llb_data_t* description) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  auto& b = builder->getBuilder();
  b.configureCommandDescription(*(Command*)command_p, toStringRef(description),
                                b.getContext());
}

bool llb_buildsystem_description_builder_set_command_attribute(
    llb_buildsystem_description_builder_t* builder_p,
    llb_buildsystem_command_t* command_p, const llb_data_t* name,
    const llb_data_t* value) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  auto& b = builder->getBuilder();
  return b.configureCommandAttribute(*(Command*)command_p, toStringRef(name),
                                     toStringRef(value), b.getContext());
}

bool llb_buildsystem_description_builder_set_command_attribute_list(
    llb_buildsystem_description_builder_t* builder_p,
    llb_buildsystem_command_t* command_p, const llb_data_t* name,
    const llb_data_t* values, uint64_t values_count) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  auto& b = builder->getBuilder();
  return b.configureCommandAttribute(*(Command*)command_p, toStringRef(name),
                                     toStringRefs(values, values_count),
                                     b.getContext());
}

bool llb_buildsystem_description_builder_set_command_attribute_map(
    llb_buildsystem_description_builder_t* builder_p,
    llb_buildsystem_command_t* command_p, const llb_data_t* name,
    const llb_data_t* keys, const llb_data_t* values, uint64_t count) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  auto& b = builder->getBuilder();
  return b.configureCommandAttribute(*(Command*)command_p, toStringRef(name),
                                     toStringRefPairs(keys, values, count),
                                     b.getContext());
}

bool llb_buildsystem_description_builder_set_command_environment_base(
    llb_buildsystem_description_builder_t* builder_p,
    llb_buildsystem_command_t* command_p, const llb_data_t* base_name) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  auto& b = builder->getBuilder();
  return b.configureCommandEnvironmentBase(*(Command*)command_p,
                                           toStringRef(base_name),
                                           b.getContext());
}

void llb_buildsystem_description_builder_finish_command(
    llb_buildsystem_description_builder_t* builder_p, const llb_data_t* name,
    llb_buildsystem_command_t* command_p) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  builder->finishCommand(toStringRef(name), (Command*)command_p);
}

void llb_buildsystem_description_builder_set_perform_ownership_analysis(
    llb_buildsystem_description_builder_t* builder_p, bool value) {
  auto builder = (CAPIDescriptionBuilder*) builder_p;
  builder->getBuilder().setPerformOwnershipAnalysis(value);
}

llb_buildsystem_command_t*
llb_buildsystem_external_command_create(
    const llb_data_t* name,
    llb_buildsystem_external_command_delegate_t delegate) {
  // Check that all required methods are provided.
  assert(delegate.start);
  assert(delegate.provide_value);
  assert(delegate.execute_command || delegate.execute_command_detached);
  
  return (llb_buildsystem_command_t*) new CAPIExternalCommand(
      StringRef((const char*)name->data, name->length), delegate);
}

void llb_buildsystem_command_get_name(llb_buildsystem_command_t* command_p,
                                      llb_data_t* key_out) {
  auto command = (Command*) command_p;
  
  auto name = command->getName();
  key_out->length = name.size();
  key_out->data = (const uint8_t*) name.data();
}

bool llb_buildsystem_command_should_show_status(
    llb_buildsystem_command_t* command_p) {
  auto command = (Command*) command_p;
  return command->shouldShowStatus();
}

char* llb_buildsystem_command_get_description(
    llb_buildsystem_command_t* command_p) {
  auto command = (Command*) command_p;

  SmallString<256> result;
  command->getShortDescription(result);
  return strdup(result.c_str());
}

char* llb_buildsystem_command_get_verbose_description(
    llb_buildsystem_command_t* command_p) {
  auto command = (Command*) command_p;

  SmallString<256> result;
  command->getVerboseDescription(result);
  return strdup(result.c_str());
}

void llb_buildsystem_command_interface_task_discovered_dependency(llb_task_interface_t ti, llb_build_key_t* key) {
  auto coreti = reinterpret_cast<core::TaskInterface*>(&ti);
  coreti->discoveredDependency(((CAPIBuildKey *)key)->getInternalBuildKey().toData());
}

void llb_buildsystem_command_interface_task_needs_input(llb_task_interface_t ti, llb_build_key_t* key, uintptr_t inputID) {
  auto coreti = reinterpret_cast<core::TaskInterface*>(&ti);
  coreti->request(((CAPIBuildKey *)key)->getInternalBuildKey().toData(), inputID);
}

void llb_buildsystem_command_interface_task_needs_single_use_input(llb_task_interface_t ti, llb_build_key_t* key, uintptr_t inputID) {
  auto coreti = reinterpret_cast<core::TaskInterface*>(&ti);
  coreti->requestSingleUse(((CAPIBuildKey *)key)->getInternalBuildKey().toData(), inputID);
}

llb_build_value_file_info_t llb_buildsystem_command_interface_get_file_info(llb_buildsystem_interface_t* bi_p, const char* path) {
  auto bi = (BuildSystem*)bi_p;
  return llbuild::capi::convertFileInfo(bi->getFileSystem().getFileInfo(path));
}

bool llb_buildsystem_command_interface_spawn(llb_task_interface_t ti, llb_buildsystem_queue_job_context_t *job_context, const char * const*args, int32_t arg_count, const char * const *env_keys, const char * const *env_values, int32_t env_count, llb_data_t *working_dir, llb_buildsystem_spawn_delegate_t *delegate) {
  auto coreti = reinterpret_cast<core::TaskInterface*>(&ti);
  auto arguments = std::vector<StringRef>();
  for (int32_t i = 0; i < arg_count; i++) {
    arguments.push_back(StringRef(args[i]));
  }
  auto environment = std::vector<std::pair<StringRef, StringRef>>();
  for (int32_t i = 0; i < env_count; i++) {
    environment.push_back(std::pair<StringRef, StringRef>(StringRef(env_keys[i]), StringRef(env_values[i])));
  }
  
  std::promise<ProcessResult> p;
  auto result = p.get_future();
  auto commandCompletionFn = [&p](ProcessResult processResult) mutable {
    p.set_value(processResult);
  };
  
  class ForwardingProcessDelegate: public basic::ProcessDelegate {
    llb_buildsystem_spawn_delegate_t *delegate;
    
  public:
    ForwardingProcessDelegate(llb_buildsystem_spawn_delegate_t *delegate): delegate(delegate) {}
    
    virtual void processStarted(ProcessContext* ctx, ProcessHandle handle,
                                llbuild_pid_t pid) {
      if (delegate != NULL) {
        delegate->process_started(delegate->context, pid);
      }
    }

    virtual void processHadError(ProcessContext* ctx, ProcessHandle handle,
                                 const Twine& message) {
      if (delegate != NULL) {
        auto errStr = message.str();
        llb_data_t err{ errStr.size(), (const uint8_t*) errStr.data() };
        delegate->process_had_error(delegate->context, &err);
      }
    };

    virtual void processHadOutput(ProcessContext* ctx, ProcessHandle handle,
                                  StringRef data) {
      if (delegate != NULL) {
        llb_data_t message{ data.size(), (const uint8_t*) data.data() };
        delegate->process_had_output(delegate->context, &message);
      }
    };

    virtual void processFinished(ProcessContext* ctx, ProcessHandle handle,
                                 const ProcessResult& result) {
      if (delegate != NULL) {
        llb_buildsystem_command_extended_result_t res;
        res.result = get_command_result(result.status);
        res.exit_status = result.exitCode;
        res.pid = result.pid;
        res.utime = result.utime;
        res.stime = result.stime;
        res.maxrss = result.maxrss;
        delegate->process_finished(delegate->context, &res);
      }
    }

  };

  auto forwardingDelegate = new ForwardingProcessDelegate(delegate);
  coreti->spawn((QueueJobContext*)job_context, ArrayRef<StringRef>(arguments), ArrayRef<std::pair<StringRef, StringRef>>(environment), {true, false, StringRef((const char*)working_dir->data, working_dir->length)}, {commandCompletionFn}, forwardingDelegate);

  delete forwardingDelegate;

  return result.get().status == ProcessStatus::Succeeded;  
}

llb_quality_of_service_t llb_get_quality_of_service() {
  switch (getDefaultQualityOfService()) {
  case QualityOfService::Normal:
    return llb_quality_of_service_default;
  case QualityOfService::UserInitiated:
    return llb_quality_of_service_user_initiated;
  case QualityOfService::Utility:
    return llb_quality_of_service_utility;
  case QualityOfService::Background:
    return llb_quality_of_service_background;
  default:
    assert(0 && "unknown quality service level");
    return llb_quality_of_service_default;
  }
}

static Optional<QualityOfService> getQoSFromLLBQoS(llb_quality_of_service_t level) {
  switch (level) {
  case llb_quality_of_service_default:
    return QualityOfService::Normal;
  case llb_quality_of_service_user_initiated:
    return QualityOfService::UserInitiated;
  case llb_quality_of_service_utility:
    return QualityOfService::Utility;
  case llb_quality_of_service_background:
    return QualityOfService::Background;
  case llb_quality_of_service_unspecified:
    return None;
  default:
    assert(0 && "unknown quality service level");
    return None;
  }
}

void llb_set_quality_of_service(llb_quality_of_service_t level) {
  assert(level != llb_quality_of_service_unspecified);
  Optional<QualityOfService> qos = getQoSFromLLBQoS(level);
  if (qos.hasValue())
    setDefaultQualityOfService(qos.getValue());
}

void* llb_alloc(size_t size) { return malloc(size); }
void llb_free(void* ptr) { free(ptr); }
