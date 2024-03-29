//===- unittests/Core/MakefileDepsParserTest.cpp --------------------------===//
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

#include "llbuild/Core/MakefileDepsParser.h"

#include "gtest/gtest.h"

#include <string>
#include <vector>

using namespace llbuild;
using namespace llbuild::core;

namespace {

TEST(MakefileDepsParserTest, basic) {
  typedef std::pair<std::string, std::vector<std::string>> RuleRecord;
  typedef std::pair<std::string, uint64_t> ErrorRecord;
  struct Testactions : public MakefileDepsParser::ParseActions {
    std::vector<RuleRecord> records;
    std::vector<std::pair<std::string, uint64_t>> errors;

    virtual void error(StringRef message, uint64_t position) override {
      errors.emplace_back(message.str(), position);
    }

    virtual void actOnRuleStart(StringRef name,
                                StringRef unescapedWord) override {
      records.emplace_back(unescapedWord.str(),
                           std::vector<std::string>{});
    }

    virtual void actOnRuleDependency(StringRef dependency,
                                     StringRef unescapedWord) override {
      assert(!records.empty());
      records.back().second.push_back(unescapedWord.str());
    }
    virtual void actOnRuleEnd() override {}
  };

  Testactions actions;
  std::string input;

  // Check a basic valid input with an escape sequence.
  input = "a: b\\$c d\\\ne";
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(0U, actions.errors.size());
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("a", { "b\\$c", "d", "e" }),
            actions.records[0]);

  // Check a basic valid input.
  actions.errors.clear();
  actions.records.clear();
  input = "a: b c d";
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(0U, actions.errors.size());
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("a", { "b", "c", "d" }),
            actions.records[0]);

  // Check a valid input with various escaped spaces.
  actions.errors.clear();
  actions.records.clear();
  input = "a\\ b: a\\ b a$$b a\\b a\\#b a\\\\\\ b";
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(0U, actions.errors.size());
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("a b", {
        "a b", "a$b", "a\\b", "a#b", "a\\ b" }),
    actions.records[0]);

  // Check a basic valid input with two rules.
  actions.errors.clear();
  actions.records.clear();
  input = "a: b c d\none: two three";
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(0U, actions.errors.size());
  EXPECT_EQ(2U, actions.records.size());
  EXPECT_EQ(RuleRecord("a", { "b", "c", "d" }),
            actions.records[0]);
  EXPECT_EQ(RuleRecord("one", { "two", "three" }),
            actions.records[1]);

  // Check a basic valid input with two rules when ignoring subsequent outputs.
  actions.errors.clear();
  actions.records.clear();
  input = "a: b c d\none: two three";
  MakefileDepsParser(StringRef(input), actions, true).parse();
  EXPECT_EQ(0U, actions.errors.size());
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("a", { "b", "c", "d" }),
            actions.records[0]);

  // Check a valid input with a trailing newline.
  input = "out: \\\n  in1\n";
  actions.errors.clear();
  actions.records.clear();
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(0U, actions.errors.size());
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("out", { "in1" }),
            actions.records[0]);

  input = "out: \\\r\n  in1\n";
  actions.errors.clear();
  actions.records.clear();
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(0U, actions.errors.size());
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("out", { "in1" }),
            actions.records[0]);

  // Check error case if leading garbage.
  actions.errors.clear();
  actions.records.clear();
  input = "  $ a";
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(1U, actions.errors.size());
  EXPECT_EQ(actions.errors[0],
            ErrorRecord("unexpected character in file", 2U));
  EXPECT_EQ(0U, actions.records.size());

  // Check error case if no ':'.
  actions.errors.clear();
  actions.records.clear();
  input = "a";
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(1U, actions.errors.size());
  EXPECT_EQ(actions.errors[0],
            ErrorRecord("missing ':' following rule", 1U));
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("a", {}),
            actions.records[0]);


  // Check error case in dependency list.
  actions.errors.clear();
  actions.records.clear();
  input = "a: b$";
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(1U, actions.errors.size());
  EXPECT_EQ(actions.errors[0],
            ErrorRecord("unexpected character in prerequisites", 4U));
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("a", { "b" }),
            actions.records[0]);

  // Check that we can parse filenames with special characters.
  actions.errors.clear();
  actions.records.clear();
  input = "/=>\\ ;|%.o : /=>\\ ;|%.swift";
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(0U, actions.errors.size());
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("/=> ;|%.o", { "/=> ;|%.swift" }), actions.records[0]);
  
  // Check that we can parse filenames with colons.
  actions.errors.clear();
  actions.records.clear();
  input = "a: b:c";
  MakefileDepsParser(StringRef(input), actions, false).parse();
  EXPECT_EQ(0U, actions.errors.size());
  EXPECT_EQ(1U, actions.records.size());
  EXPECT_EQ(RuleRecord("a", { "b:c" }), actions.records[0]);
}

}
