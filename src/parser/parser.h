#pragma once

#include <memory>
#include <string>
#include <vector>

#include "parser/ast.h"
#include "parser/token.h"

namespace sthir {

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens);

  std::unique_ptr<SQLStatement> ParseStatement();
  const std::string& error_message() const { return error_msg_; }
  bool has_error() const { return !error_msg_.empty(); }

 private:
  const Token& Peek() const;
  const Token& Previous() const;
  bool IsAtEnd() const;
  const Token& Advance();
  bool Check(TokenType type) const;
  bool Match(TokenType type);
  bool Expect(TokenType type, const std::string& msg);

  // Statement parsers
  std::unique_ptr<CreateTableStmt> ParseCreateTable();
  std::unique_ptr<CreateIndexStmt> ParseCreateIndex();
  std::unique_ptr<InsertStmt> ParseInsert();
  std::unique_ptr<SelectStmt> ParseSelect();
  std::unique_ptr<UpdateStmt> ParseUpdate();
  std::unique_ptr<DeleteStmt> ParseDelete();
  std::unique_ptr<TransactionStmt> ParseTransaction();
  std::unique_ptr<AnalyzeStmt> ParseAnalyze();
  std::unique_ptr<SetStmt> ParseSet();
  std::unique_ptr<ExplainStmt> ParseExplain();

  // Expression parsers
  std::unique_ptr<Expr> ParseExpression();
  std::unique_ptr<Expr> ParseOr();
  std::unique_ptr<Expr> ParseAnd();
  std::unique_ptr<Expr> ParseNot();
  std::unique_ptr<Expr> ParseComparison();
  std::unique_ptr<Expr> ParseTerm();
  std::unique_ptr<Expr> ParseFactor();
  std::unique_ptr<Expr> ParseUnary();
  std::unique_ptr<Expr> ParsePrimary();

  void SetError(const std::string& msg, const Token& tok);

  std::vector<Token> tokens_;
  size_t current_ = 0;
  std::string error_msg_;
};

}  // namespace sthir
