//===-- BuildFile.cpp -----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/BuildNode.h"

#include "llbuild/BuildSystem/BuildDescriptionBuilder.h"

#include "llbuild/Basic/FileSystem.h"
#include "llbuild/Basic/LLVM.h"
#include "llbuild/BuildSystem/BuildDescription.h"
#include "llbuild/BuildSystem/Command.h"
#include "llbuild/BuildSystem/Tool.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/Path.h"

#include "llbuild/Basic/PlatformUtility.h"

#include<algorithm>
#include<vector>

using namespace llbuild;
using namespace llbuild::buildsystem;

void ConfigureContext::error(const Twine& message) const {
  delegate.error(filename, at, message);
}

BuildFileDelegate::~BuildFileDelegate() {}

#pragma mark - BuildFile implementation

namespace {

#ifndef NDEBUG
static void dumpNode(llvm::yaml::Node* node, unsigned indent=0)
    LLVM_ATTRIBUTE_USED;
static void dumpNode(llvm::yaml::Node* node, unsigned indent) {
  switch (node->getType()) {
  default: {
    fprintf(stderr, "%*s<node: %p, unknown>\n", indent*2, "", node);
    break;
  }

  case llvm::yaml::Node::NK_Null: {
    fprintf(stderr, "%*s(null)\n", indent*2, "");
    break;
  }

  case llvm::yaml::Node::NK_Scalar: {
    llvm::yaml::ScalarNode* scalar = llvm::cast<llvm::yaml::ScalarNode>(node);
    SmallString<256> storage;
    fprintf(stderr, "%*s(scalar: '%s')\n", indent*2, "",
            scalar->getValue(storage).str().c_str());
    break;
  }

  case llvm::yaml::Node::NK_KeyValue: {
    assert(0 && "unexpected keyvalue node");
    break;
  }

  case llvm::yaml::Node::NK_Mapping: {
    llvm::yaml::MappingNode* map = llvm::cast<llvm::yaml::MappingNode>(node);
    fprintf(stderr, "%*smap:\n", indent*2, "");
    for (auto& it: *map) {
      fprintf(stderr, "%*skey:\n", (indent+1)*2, "");
      dumpNode(it.getKey(), indent+2);
      fprintf(stderr, "%*svalue:\n", (indent+1)*2, "");
      dumpNode(it.getValue(), indent+2);
    }
    break;
  }

  case llvm::yaml::Node::NK_Sequence: {
    llvm::yaml::SequenceNode* sequence =
      llvm::cast<llvm::yaml::SequenceNode>(node);
    fprintf(stderr, "%*ssequence:\n", indent*2, "");
    for (auto& it: *sequence) {
      dumpNode(&it, indent+1);
    }
    break;
  }

  case llvm::yaml::Node::NK_Alias: {
    fprintf(stderr, "%*s(alias)\n", indent*2, "");
    break;
  }
  }
}
#endif

class BuildFileImpl {
  /// The name of the main input file.
  std::string mainFilename;

  /// The build file delegate the BuildFile was configured with.
  BuildFileDelegate& delegate;

  /// Accumulates the graph as it is parsed. This class is purely the YAML
  /// front-end for it; everything about how the description is assembled lives
  /// in the builder, which the in-memory C API drives directly.
  BuildDescriptionBuilder builder;

  /// The number of parsing errors.
  int numErrors = 0;
    
  // FIXME: Factor out into a parser helper class.
  std::string stringFromScalarNode(llvm::yaml::ScalarNode* scalar) {
    SmallString<256> storage;
    return scalar->getValue(storage).str();
  }

  /// Emit an error.
  void error(StringRef filename, llvm::SMRange at,
             StringRef message) {
    BuildFileToken atToken{at.Start.getPointer(),
        unsigned(at.End.getPointer()-at.Start.getPointer())};
    delegate.error(mainFilename, atToken, message);
    ++numErrors;
  }

  void error(StringRef message) {
    error(mainFilename, {}, message);
  }
  
  void error(llvm::yaml::Node* node, StringRef message) {
    error(mainFilename, node->getSourceRange(), message);
  }

  ConfigureContext getContext(llvm::SMRange at) {
    BuildFileToken atToken{at.Start.getPointer(),
        unsigned(at.End.getPointer()-at.Start.getPointer())};
    return ConfigureContext{ delegate, mainFilename, atToken };
  }

  ConfigureContext getContext(llvm::yaml::Node *node) {
    return getContext(node->getSourceRange());
  }

  // FIXME: Factor out into a parser helper class.
  bool nodeIsScalarString(llvm::yaml::Node* node, StringRef name) {
    if (node->getType() != llvm::yaml::Node::NK_Scalar)
      return false;

    return stringFromScalarNode(
        static_cast<llvm::yaml::ScalarNode*>(node)) == name;
  }

  Tool* getOrCreateTool(StringRef name, llvm::yaml::Node* forNode) {
    auto tool = builder.getOrCreateTool(name);
    if (!tool) {
      error(forNode, "invalid tool (" + name.str() +") type in 'tools' map");
      return nullptr;
    }
    return tool;
  }

  Node* getOrCreateNode(StringRef name, bool isImplicit) {
    return builder.getOrCreateNode(name, isImplicit);
  }

  bool parseRootNode(llvm::yaml::Node* node) {
    // The root must always be a mapping.
    if (node->getType() != llvm::yaml::Node::NK_Mapping) {
      error(node, "unexpected top-level node");
      return false;
    }
    auto mapping = static_cast<llvm::yaml::MappingNode*>(node);

    // Iterate over each of the sections in the mapping.
    auto it = mapping->begin();
    if (!nodeIsScalarString(it->getKey(), "client")) {
      error(it->getKey(), "expected initial mapping key 'client'");
      return false;
    }
    if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
      error(it->getValue(), "unexpected 'client' value (expected map)");
      return false;
    }

    // Parse the client mapping.
    if (!parseClientMapping(
            static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
      return false;
    }
    ++it;

    // Parse the tools mapping, if present.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "tools")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        error(it->getValue(), "unexpected 'tools' value (expected map)");
        return false;
      }

      if (!parseToolsMapping(
              static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // Parse the targets mapping, if present.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "targets")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        error(it->getValue(), "unexpected 'targets' value (expected map)");
        return false;
      }

      if (!parseTargetsMapping(
              static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // Parse the default target, if present.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "default")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Scalar) {
        error(it->getValue(), "unexpected 'default' target value (expected scalar)");
        return false;
      }

      if (!parseDefaultTarget(
              static_cast<llvm::yaml::ScalarNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // Parse the nodes mapping, if present.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "nodes")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        error(it->getValue(), "unexpected 'nodes' value (expected map)");
        return false;
      }

      if (!parseNodesMapping(
              static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // Parse the shared environment tables, if present.
    //
    // Must precede 'commands', which reference these by name.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "env-bases")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        error(it->getValue(), "unexpected 'env-bases' value (expected map)");
        return false;
      }

      if (!parseEnvironmentBasesMapping(
              static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // Parse the commands mapping, if present.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "commands")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        error(it->getValue(), "unexpected 'commands' value (expected map)");
        return false;
      }

      if (!parseCommandsMapping(
              static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // There shouldn't be any trailing sections.
    if (it != mapping->end()) {
      error(&*it, "unexpected trailing top-level section");
      return false;
    }

    return true;
  }

  bool parseClientMapping(llvm::yaml::MappingNode* map) {
    // Collect all of the keys.
    std::string name;
    uint32_t version = 0;
    property_list_type properties;

    for (auto& entry: *map) {
      // All keys and values must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        error(entry.getKey(), "invalid key type in 'client' map");
        return false;
      }
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Scalar) {
        error(entry.getValue(), "invalid value type in 'client' map");
        return false;
      }

      std::string key = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      std::string value = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getValue()));
      if (key == "name") {
        name = value;
      } else if (key == "version") {
        if (StringRef(value).getAsInteger(10, version)) {
          error(entry.getValue(), "invalid version number in 'client' map");
        }
      } if (key == "perform-ownership-analysis") {
        if (value == "yes") {
          builder.setPerformOwnershipAnalysis(true);
        }
      } else {
        properties.push_back({ key, value });
      }
    }

    // Pass to the delegate.
    if (!delegate.configureClient(getContext(map), name, version, properties)) {
      error(map, "unable to configure client");
      return false;
    }

    return true;
  }

  bool parseToolsMapping(llvm::yaml::MappingNode* map) {
    for (auto& entry: *map) {
      // Every key must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        error(entry.getKey(), "invalid key type in 'tools' map");
        continue;
      }
      // Every value must be a mapping.
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        error(entry.getValue(), "invalid value type in 'tools' map");
        continue;
      }

      std::string name = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      llvm::yaml::MappingNode* attrs = static_cast<llvm::yaml::MappingNode*>(
          entry.getValue());

      // Get the tool.
      auto tool = getOrCreateTool(name, entry.getKey());
      if (!tool) {
        return false;
      }

      // Configure all of the tool attributes.
      for (auto& valueEntry: *attrs) {
        auto key = valueEntry.getKey();
        auto value = valueEntry.getValue();
        
        // All keys must be scalar.
        if (key->getType() != llvm::yaml::Node::NK_Scalar) {
          error(key, "invalid key type for tool in 'tools' map");
          continue;
        }


        auto attribute = stringFromScalarNode(
            static_cast<llvm::yaml::ScalarNode*>(key));

        if (value->getType() == llvm::yaml::Node::NK_Mapping) {
          std::vector<std::pair<std::string, std::string>> values;
          for (auto& entry: *static_cast<llvm::yaml::MappingNode*>(value)) {
            // Every key must be scalar.
            if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
              error(entry.getKey(), ("invalid key type for '" + attribute +
                                     "' in 'tools' map"));
              continue;
            }
            // Every value must be scalar.
            if (entry.getValue()->getType() != llvm::yaml::Node::NK_Scalar) {
              error(entry.getKey(), ("invalid value type for '" + attribute +
                                     "' in 'tools' map"));
              continue;
            }

            std::string key = stringFromScalarNode(
                static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
            std::string value = stringFromScalarNode(
                static_cast<llvm::yaml::ScalarNode*>(entry.getValue()));
            values.push_back(std::make_pair(key, value));
          }

          if (!tool->configureAttribute(
                  getContext(key), attribute,
                  std::vector<std::pair<StringRef, StringRef>>(
                      values.begin(), values.end()))) {
            return false;
          }
        } else if (value->getType() == llvm::yaml::Node::NK_Sequence) {
          std::vector<std::string> values;
          for (auto& node: *static_cast<llvm::yaml::SequenceNode*>(value)) {
            if (node.getType() != llvm::yaml::Node::NK_Scalar) {
              error(&node, "invalid value type for tool in 'tools' map");
              continue;
            }
            values.push_back(
                stringFromScalarNode(
                    static_cast<llvm::yaml::ScalarNode*>(&node)));
          }

          if (!tool->configureAttribute(
                  getContext(key), attribute,
                  std::vector<StringRef>(values.begin(), values.end()))) {
            return false;
          }
        } else {
          if (value->getType() != llvm::yaml::Node::NK_Scalar) {
            error(value, "invalid value type for tool in 'tools' map");
            continue;
          }

          if (!tool->configureAttribute(
                  getContext(key), attribute,
                  stringFromScalarNode(
                      static_cast<llvm::yaml::ScalarNode*>(value)))) {
            return false;
          }
        }
      }
    }

    return true;
  }
  
  bool parseTargetsMapping(llvm::yaml::MappingNode* map) {
    for (auto& entry: *map) {
      // Every key must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        error(entry.getKey(), "invalid key type in 'targets' map");
        continue;
      }
      // Every value must be a sequence.
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Sequence) {
        error(entry.getValue(), "invalid value type in 'targets' map");
        continue;
      }

      std::string name = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      llvm::yaml::SequenceNode* nodes = static_cast<llvm::yaml::SequenceNode*>(
          entry.getValue());

      // Collect the node names; the builder resolves them and registers the
      // target (including notifying the delegate).
      std::vector<std::string> nodeNameStorage;
      for (auto& node: *nodes) {
        // All items must be scalar.
        if (node.getType() != llvm::yaml::Node::NK_Scalar) {
          error(&node, "invalid node type in 'targets' map");
          continue;
        }

        nodeNameStorage.push_back(
            stringFromScalarNode(
                static_cast<llvm::yaml::ScalarNode*>(&node)));
      }

      builder.addTarget(name, std::vector<StringRef>(nodeNameStorage.begin(),
                                                     nodeNameStorage.end()));
    }

    return true;
  }

  bool parseDefaultTarget(llvm::yaml::ScalarNode* entry) {
    std::string target = stringFromScalarNode(entry);

    if (!builder.setDefaultTarget(target)) {
      error(entry, "invalid default target, a default target should be in targets");
      return false;
    }

    return true;
  }

  bool parseNodesMapping(llvm::yaml::MappingNode* map) {
    for (auto& entry: *map) {
      // Every key must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        error(entry.getKey(), "invalid key type in 'nodes' map");
        continue;
      }
      // Every value must be a mapping.
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        error(entry.getValue(), "invalid value type in 'nodes' map");
        continue;
      }

      std::string name = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      llvm::yaml::MappingNode* attrs = static_cast<llvm::yaml::MappingNode*>(
          entry.getValue());

      // Get the node.
      //
      // FIXME: One downside of doing the lookup here is that the client cannot
      // ever make a context dependent node that can have configured properties.
      auto node = getOrCreateNode(name, /*isImplicit=*/false);

      // Configure all of the tool attributes.
      for (auto& valueEntry: *attrs) {
        auto key = valueEntry.getKey();
        auto value = valueEntry.getValue();
        
        // All keys must be scalar.
        if (key->getType() != llvm::yaml::Node::NK_Scalar) {
          error(key, "invalid key type for node in 'nodes' map");
          continue;
        }

        auto attribute = stringFromScalarNode(
            static_cast<llvm::yaml::ScalarNode*>(key));

        if (value->getType() == llvm::yaml::Node::NK_Mapping) {
          std::vector<std::pair<std::string, std::string>> values;
          for (auto& entry: *static_cast<llvm::yaml::MappingNode*>(value)) {
            // Every key must be scalar.
            if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
              error(entry.getKey(), ("invalid key type for '" + attribute +
                                     "' in 'nodes' map"));
              continue;
            }
            // Every value must be scalar.
            if (entry.getValue()->getType() != llvm::yaml::Node::NK_Scalar) {
              error(entry.getKey(), ("invalid value type for '" + attribute +
                                     "' in 'nodes' map"));
              continue;
            }

            std::string key = stringFromScalarNode(
                static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
            std::string value = stringFromScalarNode(
                static_cast<llvm::yaml::ScalarNode*>(entry.getValue()));
            values.push_back(std::make_pair(key, value));
          }

          if (!node->configureAttribute(
                  getContext(key), attribute,
                  std::vector<std::pair<StringRef, StringRef>>(
                      values.begin(), values.end()))) {
            return false;
          }
        } else if (value->getType() == llvm::yaml::Node::NK_Sequence) {
          std::vector<std::string> values;
          for (auto& node: *static_cast<llvm::yaml::SequenceNode*>(value)) {
            if (node.getType() != llvm::yaml::Node::NK_Scalar) {
              error(&node, "invalid value type for node in 'nodes' map");
              continue;
            }
            values.push_back(
                stringFromScalarNode(
                    static_cast<llvm::yaml::ScalarNode*>(&node)));
          }

          if (!node->configureAttribute(
                  getContext(key), attribute,
                  std::vector<StringRef>(values.begin(), values.end()))) {
            return false;
          }
        } else {
          if (value->getType() != llvm::yaml::Node::NK_Scalar) {
            error(value, "invalid value type for node in 'nodes' map");
            continue;
          }
        
          if (!node->configureAttribute(
                  getContext(key), attribute,
                  stringFromScalarNode(
                      static_cast<llvm::yaml::ScalarNode*>(value)))) {
            return false;
          }
        }
      }
    }

    return true;
  }

  bool parseEnvironmentBasesMapping(llvm::yaml::MappingNode* map) {
    for (auto& entry: *map) {
      // Every key must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        error(entry.getKey(), "invalid key type in 'env-bases' map");
        continue;
      }
      // Every value must be a mapping.
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        error(entry.getValue(), "invalid value type in 'env-bases' map");
        continue;
      }

      auto name = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      if (builder.hasEnvironmentBase(name)) {
        error(entry.getKey(), "duplicate environment base '" + name + "'");
        continue;
      }

      // Held only until `addEnvironmentBase` interns them.
      std::vector<std::pair<std::string, std::string>> bindingStorage;
      for (auto& binding:
               *static_cast<llvm::yaml::MappingNode*>(entry.getValue())) {
        // Every key must be scalar.
        if (binding.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
          error(binding.getKey(),
                "invalid key type in 'env-bases' entry '" + name + "'");
          continue;
        }
        // Every value must be scalar.
        if (binding.getValue()->getType() != llvm::yaml::Node::NK_Scalar) {
          error(binding.getKey(),
                "invalid value type in 'env-bases' entry '" + name + "'");
          continue;
        }

        bindingStorage.emplace_back(
            stringFromScalarNode(
                static_cast<llvm::yaml::ScalarNode*>(binding.getKey())),
            stringFromScalarNode(
                static_cast<llvm::yaml::ScalarNode*>(binding.getValue())));
      }

      std::vector<std::pair<StringRef, StringRef>> bindings(
          bindingStorage.begin(), bindingStorage.end());
      builder.addEnvironmentBase(name, bindings);
    }

    return true;
  }

  bool parseCommandsMapping(llvm::yaml::MappingNode* map) {
    for (auto& entry: *map) {
      // Every key must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        error(entry.getKey(), "invalid key type in 'commands' map");
        continue;
      }
      // Every value must be a mapping.
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        error(entry.getValue(), "invalid value type in 'commands' map");
        continue;
      }

      std::string name = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      llvm::yaml::MappingNode* attrs = static_cast<llvm::yaml::MappingNode*>(
          entry.getValue());

      // Check that the command is not a duplicate.
      if (builder.hasCommand(name)) {
        error(entry.getKey(), "duplicate command in 'commands' map");
        continue;
      }
      
      // Get the initial attribute, which must be the tool name.
      auto it = attrs->begin();
      if (it == attrs->end()) {
        error(entry.getKey(),
              "missing 'tool' key for command in 'command' map");
        continue;
      }
      if (!nodeIsScalarString(it->getKey(), "tool")) {
        error(it->getKey(),
              "expected 'tool' initial key for command in 'commands' map");
        // Skip to the end.
        while (it != attrs->end()) ++it;
        continue;
      }
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Scalar) {
        error(it->getValue(),
              "invalid 'tool' value type for command in 'commands' map");
        // Skip to the end.
        while (it != attrs->end()) ++it;
        continue;
      }
      
      // Lookup the tool for this command.
      auto tool = getOrCreateTool(
          stringFromScalarNode(
              static_cast<llvm::yaml::ScalarNode*>(
                  it->getValue())),
          it->getValue());
      if (!tool) {
        return false;
      }
        
      // Create the command.
      auto command = tool->createCommand(name);
      if (!command) {
        error(it->getValue(), "tool failed to create a command");
        return false;
      }

      // Parse the remaining command attributes.
      ++it;
      for (; it != attrs->end(); ++it) {
        auto key = it->getKey();
        auto value = it->getValue();
        
        // If this is a known key, parse it.
        if (nodeIsScalarString(key, "inputs")) {
          if (value->getType() != llvm::yaml::Node::NK_Sequence) {
            error(value, "invalid value type for 'inputs' command key");
            continue;
          }

          llvm::yaml::SequenceNode* nodeNames =
            static_cast<llvm::yaml::SequenceNode*>(value);

          std::vector<Node*> nodes;
          for (auto& nodeName: *nodeNames) {
            if (nodeName.getType() != llvm::yaml::Node::NK_Scalar) {
              error(&nodeName, "invalid node type in 'inputs' command key");
              continue;
            }

            nodes.push_back(
                getOrCreateNode(
                    stringFromScalarNode(
                        static_cast<llvm::yaml::ScalarNode*>(&nodeName)),
                    /*isImplicit=*/true));
          }

          builder.configureCommandInputs(*command, nodes, getContext(key));
        } else if (nodeIsScalarString(key, "outputs")) {
          if (value->getType() != llvm::yaml::Node::NK_Sequence) {
            error(value, "invalid value type for 'outputs' command key");
            continue;
          }

          llvm::yaml::SequenceNode* nodeNames =
            static_cast<llvm::yaml::SequenceNode*>(value);

          std::vector<Node*> nodes;
          for (auto& nodeName: *nodeNames) {
            if (nodeName.getType() != llvm::yaml::Node::NK_Scalar) {
              error(&nodeName, "invalid node type in 'outputs' command key");
              continue;
            }

            nodes.push_back(
                getOrCreateNode(
                    stringFromScalarNode(
                        static_cast<llvm::yaml::ScalarNode*>(&nodeName)),
                    /*isImplicit=*/true));
          }

          // This also records the command as a producer of each output.
          builder.configureCommandOutputs(*command, nodes, getContext(key));
        } else if (nodeIsScalarString(key, "description")) {
          if (value->getType() != llvm::yaml::Node::NK_Scalar) {
            error(value, "invalid value type for 'description' command key");
            continue;
          }

          command->configureDescription(
              getContext(key), stringFromScalarNode(
                  static_cast<llvm::yaml::ScalarNode*>(value)));
        } else if (nodeIsScalarString(key, "env-base")) {
          if (value->getType() != llvm::yaml::Node::NK_Scalar) {
            error(value, "invalid value type for 'env-base' command key");
            continue;
          }

          auto baseName = stringFromScalarNode(
              static_cast<llvm::yaml::ScalarNode*>(value));
          auto base = builder.lookupEnvironmentBase(baseName);
          if (!base) {
            error(value, "unknown environment base '" + baseName + "'");
            continue;
          }

          if (!command->configureEnvironmentBase(getContext(key), base)) {
            error(key, "'env-base' is not supported by this command");
            return false;
          }
        } else {
          // Otherwise, it should be an attribute assignment.
          
          // All keys must be scalar.
          if (key->getType() != llvm::yaml::Node::NK_Scalar) {
            error(key, "invalid key type in 'commands' map");
            continue;
          }

          auto attribute = stringFromScalarNode(
              static_cast<llvm::yaml::ScalarNode*>(key));

          if (value->getType() == llvm::yaml::Node::NK_Mapping) {
            std::vector<std::pair<std::string, std::string>> values;
            for (auto& entry: *static_cast<llvm::yaml::MappingNode*>(value)) {
              // Every key must be scalar.
              if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
                error(entry.getKey(), ("invalid key type for '" + attribute +
                                       "' in 'commands' map"));
                continue;
              }
              // Every value must be scalar.
              if (entry.getValue()->getType() != llvm::yaml::Node::NK_Scalar) {
                error(entry.getKey(), ("invalid value type for '" + attribute +
                                       "' in 'commands' map"));
                continue;
              }

              std::string key = stringFromScalarNode(
                  static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
              std::string value = stringFromScalarNode(
                  static_cast<llvm::yaml::ScalarNode*>(entry.getValue()));
              values.push_back(std::make_pair(key, value));
            }

            if (!command->configureAttribute(
                    getContext(key), attribute,
                    std::vector<std::pair<StringRef, StringRef>>(
                        values.begin(), values.end()))) {
              return false;
            }
          } else if (value->getType() == llvm::yaml::Node::NK_Sequence) {
            std::vector<std::string> values;
            for (auto& node: *static_cast<llvm::yaml::SequenceNode*>(value)) {
              if (node.getType() != llvm::yaml::Node::NK_Scalar) {
                error(&node, "invalid value type for command in 'commands' map");
                continue;
              }
              values.push_back(
                  stringFromScalarNode(
                      static_cast<llvm::yaml::ScalarNode*>(&node)));
            }

            if (!command->configureAttribute(
                    getContext(key), attribute,
                    std::vector<StringRef>(values.begin(), values.end()))) {
              return false;
            }
          } else {
            if (value->getType() != llvm::yaml::Node::NK_Scalar) {
              error(value, "invalid value type for command in 'commands' map");
              continue;
            }
            
            if (!command->configureAttribute(
                    getContext(key), attribute,
                    stringFromScalarNode(
                        static_cast<llvm::yaml::ScalarNode*>(value)))) {
              return false;
            }
          }
        }
      }

      builder.addCommand(name, std::move(command));
    }

    return true;
  }

public:
  BuildFileImpl(class BuildFile& buildFile,
                StringRef mainFilename,
                BuildFileDelegate& delegate)
    : mainFilename(mainFilename), delegate(delegate),
      builder(delegate, mainFilename) {}

  BuildFileDelegate* getDelegate() {
    return &delegate;
  }

  /// @name Parse Actions
  /// @{

  std::unique_ptr<BuildDescription> load() {
    // Create a memory buffer for the input.
    //
    // FIXME: Lift the file access into the delegate, like we do for Ninja.
    llvm::SourceMgr sourceMgr;
    auto input = delegate.getFileSystem().getFileContents(mainFilename);
    if (!input) {
      error("unable to open '" + mainFilename + "'");
      return nullptr;
    }

    delegate.setFileContentsBeingParsed(input->getBuffer());

    // Create a YAML parser.
    llvm::yaml::Stream stream(input->getMemBufferRef(), sourceMgr);

    // Read the stream, we only expect a single document.
    auto it = stream.begin();
    if (it == stream.end()) {
      error("missing document in stream");
      return nullptr;
    }

    auto& document = *it;
    auto root = document.getRoot();
    if (!root) {
      error("missing document in stream");
      return nullptr;
    }

    if (!parseRootNode(root)) {
      return nullptr;
    }

    if (++it != stream.end()) {
      error(it->getRoot(), "unexpected additional document in stream");
      return nullptr;
    }

    return builder.finalize();
  }
};

}

#pragma mark - BuildFile

BuildFile::BuildFile(StringRef mainFilename,
                     BuildFileDelegate& delegate)
  : impl(new BuildFileImpl(*this, mainFilename, delegate))
{
}

BuildFile::~BuildFile() {
  delete static_cast<BuildFileImpl*>(impl);
}

std::unique_ptr<BuildDescription> BuildFile::load() {
  // Create the build description.
  return static_cast<BuildFileImpl*>(impl)->load();
}
