#pragma once

#include <string>
#include <vector>

#include "parser/token.h"

namespace sthir {

class Lexer {
 public:
  explicit Lexer(std::string source);

  std::vector<Token> Tokenize();
  const std::string& error_message() const { return error_msg_; }
  bool has_error() const { return !error_msg_.empty(); }

 private:
  char Peek() const;
  char PeekNext() const;
  char Advance();
  bool Match(char expected);
  void SkipWhitespaceAndComments();
  Token ScanToken();
  Token ScanIdentifierOrKeyword();
  Token ScanNumber();
  Token ScanString();

  std::string source_;
  size_t start_ = 0;
  size_t current_ = 0;
  size_t line_ = 1;
  size_t column_ = 1;
  size_t start_col_ = 1;
  std::string error_msg_;
};

}  // namespace sthir
