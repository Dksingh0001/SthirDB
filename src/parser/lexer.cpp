#include "parser/lexer.h"

#include <cctype>
#include <unordered_map>
#include <algorithm>

namespace sthir {

std::string TokenTypeToString(TokenType type) {
  switch (type) {
    case TokenType::CREATE: return "CREATE";
    case TokenType::TABLE: return "TABLE";
    case TokenType::INT: return "INT";
    case TokenType::INTEGER: return "INTEGER";
    case TokenType::VARCHAR: return "VARCHAR";
    case TokenType::DECIMAL: return "DECIMAL";
    case TokenType::INSERT: return "INSERT";
    case TokenType::INTO: return "INTO";
    case TokenType::VALUES: return "VALUES";
    case TokenType::SELECT: return "SELECT";
    case TokenType::FROM: return "FROM";
    case TokenType::WHERE: return "WHERE";
    case TokenType::GROUP: return "GROUP";
    case TokenType::BY: return "BY";
    case TokenType::ORDER: return "ORDER";
    case TokenType::ASC: return "ASC";
    case TokenType::DESC: return "DESC";
    case TokenType::UPDATE: return "UPDATE";
    case TokenType::SET: return "SET";
    case TokenType::DELETE: return "DELETE";
    case TokenType::JOIN: return "JOIN";
    case TokenType::ON: return "ON";
    case TokenType::AS: return "AS";
    case TokenType::COUNT: return "COUNT";
    case TokenType::SUM: return "SUM";
    case TokenType::MIN: return "MIN";
    case TokenType::MAX: return "MAX";
    case TokenType::AVG: return "AVG";
    case TokenType::AND: return "AND";
    case TokenType::OR: return "OR";
    case TokenType::NOT: return "NOT";
    case TokenType::IS: return "IS";
    case TokenType::NULL_KW: return "NULL";
    case TokenType::BEGIN_KW: return "BEGIN";
    case TokenType::COMMIT: return "COMMIT";
    case TokenType::ABORT: return "ABORT";
    case TokenType::ROLLBACK: return "ROLLBACK";
    case TokenType::TRANSACTION: return "TRANSACTION";
    case TokenType::CHECKPOINT: return "CHECKPOINT";
    case TokenType::ANALYZE: return "ANALYZE";
    case TokenType::EXPLAIN: return "EXPLAIN";
    case TokenType::PRIMARY: return "PRIMARY";
    case TokenType::KEY: return "KEY";
    case TokenType::UNIQUE: return "UNIQUE";
    case TokenType::CHECK: return "CHECK";
    case TokenType::FOREIGN: return "FOREIGN";
    case TokenType::REFERENCES: return "REFERENCES";
    case TokenType::INDEX: return "INDEX";
    case TokenType::RESTRICT: return "RESTRICT";
    case TokenType::CASCADE: return "CASCADE";
    case TokenType::LPAREN: return "(";
    case TokenType::RPAREN: return ")";
    case TokenType::COMMA: return ",";
    case TokenType::SEMICOLON: return ";";
    case TokenType::DOT: return ".";
    case TokenType::STAR: return "*";
    case TokenType::EQ: return "=";
    case TokenType::NEQ: return "<>";
    case TokenType::LT: return "<";
    case TokenType::LTE: return "<=";
    case TokenType::GT: return ">";
    case TokenType::GTE: return ">=";
    case TokenType::IDENTIFIER: return "IDENTIFIER";
    case TokenType::INTEGER_LITERAL: return "INTEGER_LITERAL";
    case TokenType::DECIMAL_LITERAL: return "DECIMAL_LITERAL";
    case TokenType::STRING_LITERAL: return "STRING_LITERAL";
    case TokenType::END_OF_FILE: return "EOF";
    case TokenType::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}

namespace {

const std::unordered_map<std::string, TokenType> kKeywords = {
    {"create", TokenType::CREATE},
    {"table", TokenType::TABLE},
    {"int", TokenType::INT},
    {"integer", TokenType::INTEGER},
    {"varchar", TokenType::VARCHAR},
    {"decimal", TokenType::DECIMAL},
    {"insert", TokenType::INSERT},
    {"into", TokenType::INTO},
    {"values", TokenType::VALUES},
    {"select", TokenType::SELECT},
    {"from", TokenType::FROM},
    {"where", TokenType::WHERE},
    {"group", TokenType::GROUP},
    {"by", TokenType::BY},
    {"order", TokenType::ORDER},
    {"asc", TokenType::ASC},
    {"desc", TokenType::DESC},
    {"update", TokenType::UPDATE},
    {"set", TokenType::SET},
    {"delete", TokenType::DELETE},
    {"join", TokenType::JOIN},
    {"on", TokenType::ON},
    {"as", TokenType::AS},
    {"count", TokenType::COUNT},
    {"sum", TokenType::SUM},
    {"min", TokenType::MIN},
    {"max", TokenType::MAX},
    {"avg", TokenType::AVG},
    {"and", TokenType::AND},
    {"or", TokenType::OR},
    {"not", TokenType::NOT},
    {"is", TokenType::IS},
    {"null", TokenType::NULL_KW},
    {"begin", TokenType::BEGIN_KW},
    {"commit", TokenType::COMMIT},
    {"abort", TokenType::ABORT},
    {"rollback", TokenType::ROLLBACK},
    {"transaction", TokenType::TRANSACTION},
    {"checkpoint", TokenType::CHECKPOINT},
    {"analyze", TokenType::ANALYZE},
    {"explain", TokenType::EXPLAIN},
    {"primary", TokenType::PRIMARY},
    {"key", TokenType::KEY},
    {"unique", TokenType::UNIQUE},
    {"check", TokenType::CHECK},
    {"foreign", TokenType::FOREIGN},
    {"references", TokenType::REFERENCES},
    {"index", TokenType::INDEX},
    {"restrict", TokenType::RESTRICT},
    {"cascade", TokenType::CASCADE},
};

}  // namespace

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

char Lexer::Peek() const {
  if (current_ >= source_.size()) return '\0';
  return source_[current_];
}

char Lexer::PeekNext() const {
  if (current_ + 1 >= source_.size()) return '\0';
  return source_[current_ + 1];
}

char Lexer::Advance() {
  if (current_ >= source_.size()) return '\0';
  char c = source_[current_++];
  ++column_;
  return c;
}

bool Lexer::Match(char expected) {
  if (Peek() != expected) return false;
  Advance();
  return true;
}

void Lexer::SkipWhitespaceAndComments() {
  while (current_ < source_.size()) {
    char c = Peek();
    if (c == ' ' || c == '\t' || c == '\r') {
      Advance();
    } else if (c == '\n') {
      Advance();
      ++line_;
      column_ = 1;
    } else if (c == '-' && PeekNext() == '-') {
      // Single-line comment
      while (current_ < source_.size() && Peek() != '\n') {
        Advance();
      }
    } else if (c == '/' && PeekNext() == '*') {
      // Multi-line comment
      Advance(); Advance();
      while (current_ < source_.size() && !(Peek() == '*' && PeekNext() == '/')) {
        if (Peek() == '\n') {
          ++line_;
          column_ = 0;
        }
        Advance();
      }
      if (current_ < source_.size()) {
        Advance(); Advance();
      }
    } else {
      break;
    }
  }
}

Token Lexer::ScanIdentifierOrKeyword() {
  while (std::isalnum(static_cast<unsigned char>(Peek())) || Peek() == '_') {
    Advance();
  }
  std::string lexeme = source_.substr(start_, current_ - start_);
  std::string lower = lexeme;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  TokenType type = TokenType::IDENTIFIER;
  auto it = kKeywords.find(lower);
  if (it != kKeywords.end()) {
    type = it->second;
  }
  return Token{type, lexeme, line_, start_col_};
}

Token Lexer::ScanNumber() {
  bool is_decimal = false;
  while (std::isdigit(static_cast<unsigned char>(Peek()))) {
    Advance();
  }
  if (Peek() == '.' && std::isdigit(static_cast<unsigned char>(PeekNext()))) {
    is_decimal = true;
    Advance();  // consume '.'
    while (std::isdigit(static_cast<unsigned char>(Peek()))) {
      Advance();
    }
  }
  std::string lexeme = source_.substr(start_, current_ - start_);
  return Token{is_decimal ? TokenType::DECIMAL_LITERAL : TokenType::INTEGER_LITERAL,
               lexeme, line_, start_col_};
}

Token Lexer::ScanString() {
  std::string val;
  while (current_ < source_.size()) {
    char c = Peek();
    if (c == '\'') {
      Advance();
      if (Peek() == '\'') {
        // Escaped single quote: ''
        val.push_back('\'');
        Advance();
      } else {
        // End of string
        return Token{TokenType::STRING_LITERAL, val, line_, start_col_};
      }
    } else if (c == '\n') {
      val.push_back(c);
      Advance();
      ++line_;
      column_ = 1;
    } else {
      val.push_back(c);
      Advance();
    }
  }
  error_msg_ = "Unterminated string starting at line " + std::to_string(line_) + ", col " + std::to_string(start_col_);
  return Token{TokenType::UNKNOWN, val, line_, start_col_};
}

Token Lexer::ScanToken() {
  SkipWhitespaceAndComments();
  start_ = current_;
  start_col_ = column_;

  if (current_ >= source_.size()) {
    return Token{TokenType::END_OF_FILE, "", line_, column_};
  }

  char c = Advance();

  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    return ScanIdentifierOrKeyword();
  }
  if (std::isdigit(static_cast<unsigned char>(c))) {
    return ScanNumber();
  }
  if (c == '\'') {
    return ScanString();
  }

  switch (c) {
    case '(': return Token{TokenType::LPAREN, "(", line_, start_col_};
    case ')': return Token{TokenType::RPAREN, ")", line_, start_col_};
    case ',': return Token{TokenType::COMMA, ",", line_, start_col_};
    case ';': return Token{TokenType::SEMICOLON, ";", line_, start_col_};
    case '.': return Token{TokenType::DOT, ".", line_, start_col_};
    case '*': return Token{TokenType::STAR, "*", line_, start_col_};
    case '=': return Token{TokenType::EQ, "=", line_, start_col_};
    case '<':
      if (Match('>')) return Token{TokenType::NEQ, "<>", line_, start_col_};
      if (Match('=')) return Token{TokenType::LTE, "<=", line_, start_col_};
      return Token{TokenType::LT, "<", line_, start_col_};
    case '>':
      if (Match('=')) return Token{TokenType::GTE, ">=", line_, start_col_};
      return Token{TokenType::GT, ">", line_, start_col_};
    case '!':
      if (Match('=')) return Token{TokenType::NEQ, "!=", line_, start_col_};
      error_msg_ = "Unexpected character '!' at line " + std::to_string(line_) + ", col " + std::to_string(start_col_);
      return Token{TokenType::UNKNOWN, "!", line_, start_col_};
    default:
      error_msg_ = std::string("Unexpected character '") + c + "' at line " + std::to_string(line_) + ", col " + std::to_string(start_col_);
      return Token{TokenType::UNKNOWN, std::string(1, c), line_, start_col_};
  }
}

std::vector<Token> Lexer::Tokenize() {
  std::vector<Token> tokens;
  while (true) {
    Token tok = ScanToken();
    tokens.push_back(tok);
    if (tok.type == TokenType::END_OF_FILE || tok.type == TokenType::UNKNOWN) {
      break;
    }
  }
  return tokens;
}

}  // namespace sthir
