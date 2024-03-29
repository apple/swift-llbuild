//===-- MakefileDepsParser.cpp --------------------------------------------===//
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

#include "llvm/ADT/SmallString.h"

using namespace llvm;
using namespace llbuild;
using namespace llbuild::core;

MakefileDepsParser::ParseActions::~ParseActions() {}

#pragma mark - MakefileDepsParser Implementation

static bool isWordChar(int c) {
  switch (c) {
  case '\0':
  case '\t':
  case '\r':
  case '\n':
  case ' ':
  case '$':
  case ':':
    return false;
  default:
    return true;
  }
}

static void skipWhitespaceAndComments(const char*& cur, const char* end) {
  for (; cur != end; ++cur) {
    int c = *cur;

    // Skip comments.
    if (c == '#') {
      // Skip to the next newline.
      while (cur + 1 != end && cur[1] == '\n')
        ++cur;
      continue;
    }

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      continue;

    break;
  }
}

static void skipNonNewlineWhitespace(const char*& cur, const char* end) {
  for (; cur != end; ++cur) {
    int c = *cur;

    // Skip regular whitespace.
    if (c == ' ' || c == '\t' || c == '\r')
      continue;

    // If this is an escaped newline, also skip it.
    if (c == '\\' && cur + 1 != end && cur[1] == '\n') {
      ++cur;
      continue;
    }

    // Also skip \r\n newlines
    if (c == '\\' && cur + 2 < end && cur[1] == '\r' && cur[2] == '\n') {
      cur += 2;
      continue;
    }

    // Otherwise, stop scanning.
    break;
  }
}

static void skipToEndOfLine(const char*& cur, const char* end) {
  for (; cur != end; ++cur) {
    int c = *cur;

    if (c == '\n') {
      ++cur;
      break;
    }
  }
}

static void lexWord(const char*& cur, const char* end,
                    SmallVectorImpl<char> &unescapedWord) {
  for (; cur != end; ++cur) {
    int c = *cur;

    // Check if this is an escape sequence.
    if (c == '\\') {
      // If this is a line continuation, it ends the word.
      if (cur + 1 != end && cur[1] == '\n')
        break;

      // Otherwise, skip the escaped character.
      ++cur;
      int c = *cur;

      // Honor the escaping rules as generated by Clang and GCC, which are *not
      // necessarily* the actual escaping rules of BSD Make or GNU Make. Due to
      // incompatibilities in the escape syntax between those two makes, and the
      // GNU make behavior of retrying an escaped string with the original
      // input, GCC/Clang adopt a strategy around escaping ' ' and '#', but not
      // r"\\" itself, or any other characters. However, there are some
      // situations where Clang *will* generate an escaped '\\' using the
      // r"\\\\" sequence, so we also honor that.
      //
      // FIXME: Make this more complete, or move to a better dependency format.
      if (c == ' ' || c == '#' || c == '\\') {
        unescapedWord.push_back(c);
      } else {
        unescapedWord.push_back('\\');
        unescapedWord.push_back(c);
      }
      continue;
    } else if (c == '$' && cur + 1 != end && cur[1] == '$') {
      // "$$" is an escaped '$'.
      unescapedWord.push_back(c);
      ++cur;
      continue;
    }

    // Otherwise, if this is not a valid word character then skip it.
    if (!isWordChar(c)) {
#if defined(_WIN32)
      // If we encounter a colon and it looks like a driver letter separator, use
      // it as that instead of separating the word.
      if (c == ':' && cur + 1 != end && (*(cur + 1) == '/' || *(cur + 1) == '\\')) {
        unescapedWord.push_back(':');
        continue;
      }
#endif
      break;
    }
    unescapedWord.push_back(c);
  }
}

void MakefileDepsParser::parse() {
  const char* cur = data.data();
  const char* end = cur + data.size();
  // Storage for currently begin lexed unescaped word.
  SmallString<256> unescapedWord;

  // While we have input data...
  while (cur != end) {
    // Skip leading whitespace and comments.
    skipWhitespaceAndComments(cur, end);

    // If we have reached the end of the input, we are done.
    if (cur == end)
      break;
    
    // The next token should be a word.
    const char* wordStart = cur;
    unescapedWord.clear();
    lexWord(cur, end, unescapedWord);
    if (cur == wordStart) {
      actions.error("unexpected character in file", cur - data.data());
      skipToEndOfLine(cur, end);
      continue;
    }
    actions.actOnRuleStart(StringRef(wordStart, cur - wordStart),
                           unescapedWord.str());

    // The next token should be a colon.
    skipNonNewlineWhitespace(cur, end);
    if (cur == end || *cur != ':') {
      actions.error("missing ':' following rule", cur - data.data());
      actions.actOnRuleEnd();
      skipToEndOfLine(cur, end);
      continue;
    }

    // Skip the colon.
    ++cur;

    // Consume dependency words until we reach the end of a line.
    while (cur != end) {
      // Skip forward and check for EOL.
      skipNonNewlineWhitespace(cur, end);
      if (cur == end || *cur == '\n')
        break;

      // Otherwise, we should have a word.
      const char* wordStart = cur;
      unescapedWord.clear();
      lexWord(cur, end, unescapedWord);
      if (cur == wordStart) {
        actions.error("unexpected character in prerequisites",
                      cur - data.data());
        skipToEndOfLine(cur, end);
        continue;
      }
      // Given that GCC/Clang generally do not escape any special characters in
      // paths, we may encounter paths that contain them. Our lexer
      // automatically stops on some such characters, namely ':'. If the we find
      // a ':' at this point, we push it onto the word and continue lexing.
      while (cur != end && *cur == ':') {
        unescapedWord.push_back(*cur);
        ++cur;
        lexWord(cur, end, unescapedWord);
      }
      actions.actOnRuleDependency(StringRef(wordStart, cur - wordStart),
                                  unescapedWord.str());
    }
    actions.actOnRuleEnd();

    // Stop parsing if we only care about the first output's dependencies.
    if (ignoreSubsequentOutputs) break;
  }
}
