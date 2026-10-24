#pragma once

#include <string>

namespace sthir {

enum class TokenType {
  // Keywords
  CREATE, TABLE, INT, INTEGER, VARCHAR, DECIMAL,
  INSERT, INTO, VALUES,
  SELECT, FROM, WHERE, GROUP, BY, ORDER, ASC, DESC,
  UPDATE, SET, DELETE,
  JOIN, ON, AS,
  COUNT, SUM, MIN, MAX, AVG,
  AND, OR, NOT, IS, NULL_KW,
  BEGIN_KW, COMMIT, ABORT, ROLLBACK, TRANSACTION,
  ANALYZE, EXPLAIN,
  PRIMARY, KEY, UNIQUE, CHECK, FOREIGN, REFERENCES, INDEX,
  RESTRICT, CASCADE,

  // Symbols & Operators
  LPAREN, RPAREN, COMMA, SEMICOLON, DOT, STAR,
  EQ, NEQ, LT, LTE, GT, GTE,

  // Literals & Identifiers
  IDENTIFIER,
  INTEGER_LITERAL,
  DECIMAL_LITERAL,
  STRING_LITERAL,

  // Special
  END_OF_FILE,
  UNKNOWN
};

struct Token {
  TokenType type = TokenType::UNKNOWN;
  std::string lexeme;
  size_t line = 1;
  size_t column = 1;
};

std::string TokenTypeToString(TokenType type);

}  // namespace sthir
