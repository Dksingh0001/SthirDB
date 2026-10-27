#include "parser/parser.h"

#include <cctype>
#include <sstream>

namespace sthir {

std::vector<std::string> SplitSqlScript(const std::string& script) {
  std::vector<std::string> statements;
  std::string current;
  bool in_string = false;
  bool in_comment = false;
  for (size_t i = 0; i < script.size(); ++i) {
    const char c = script[i];
    if (in_comment) {
      if (c == '\n') {
        in_comment = false;
        current.push_back(' ');
      }
      continue;
    }
    if (!in_string && c == '-' && i + 1 < script.size() && script[i + 1] == '-') {
      in_comment = true;
      ++i;
      continue;
    }
    if (c == '\'') {
      in_string = !in_string;
      current.push_back(c);
      continue;
    }
    if (c == ';' && !in_string) {
      statements.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) statements.push_back(current);

  // Drop statements that are empty or only whitespace/comments.
  std::vector<std::string> nonempty;
  for (auto& s : statements) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) nonempty.push_back(s);
  }
  return nonempty;
}

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::Peek() const {
  if (current_ >= tokens_.size()) {
    static const Token eof_token{TokenType::END_OF_FILE, "", 0, 0};
    return eof_token;
  }
  return tokens_[current_];
}

const Token& Parser::Previous() const {
  if (current_ == 0) return tokens_[0];
  return tokens_[current_ - 1];
}

bool Parser::IsAtEnd() const {
  return Peek().type == TokenType::END_OF_FILE;
}

const Token& Parser::Advance() {
  if (!IsAtEnd()) ++current_;
  return Previous();
}

bool Parser::Check(TokenType type) const {
  if (IsAtEnd()) return false;
  return Peek().type == type;
}

bool Parser::Match(TokenType type) {
  if (Check(type)) {
    Advance();
    return true;
  }
  return false;
}

void Parser::SetError(const std::string& msg, const Token& tok) {
  if (error_msg_.empty()) {
    std::ostringstream ss;
    ss << "Parse error at line " << tok.line << ", col " << tok.column
       << ": " << msg << " (found '" << tok.lexeme << "')";
    error_msg_ = ss.str();
  }
}

bool Parser::Expect(TokenType type, const std::string& msg) {
  if (Check(type)) {
    Advance();
    return true;
  }
  SetError(msg, Peek());
  return false;
}

std::unique_ptr<SQLStatement> Parser::ParseStatement() {
  if (IsAtEnd()) {
    SetError("unexpected end of input", Peek());
    return nullptr;
  }

  if (Match(TokenType::CREATE)) {
    if (Check(TokenType::INDEX)) return ParseCreateIndex();
    return ParseCreateTable();
  }
  if (Match(TokenType::INSERT)) return ParseInsert();
  if (Match(TokenType::SELECT)) return ParseSelect();
  if (Match(TokenType::UPDATE)) return ParseUpdate();
  if (Match(TokenType::DELETE)) return ParseDelete();
  if (Check(TokenType::BEGIN_KW) || Check(TokenType::COMMIT) ||
      Check(TokenType::ABORT) || Check(TokenType::ROLLBACK) ||
      Check(TokenType::CHECKPOINT)) {
    return ParseTransaction();
  }
  if (Match(TokenType::ANALYZE)) return ParseAnalyze();
  if (Match(TokenType::SET)) return ParseSet();
  if (Match(TokenType::EXPLAIN)) return ParseExplain();

  SetError("unrecognized statement", Peek());
  return nullptr;
}

std::unique_ptr<CreateIndexStmt> Parser::ParseCreateIndex() {
  if (!Expect(TokenType::INDEX, "expected 'INDEX' after 'CREATE'")) return nullptr;
  if (!Expect(TokenType::IDENTIFIER, "expected index name")) return nullptr;
  auto stmt = std::make_unique<CreateIndexStmt>();
  stmt->index_name = Previous().lexeme;

  if (!Expect(TokenType::ON, "expected 'ON' after index name")) return nullptr;
  if (!Expect(TokenType::IDENTIFIER, "expected table name after 'ON'")) return nullptr;
  stmt->table_name = Previous().lexeme;

  if (!Expect(TokenType::LPAREN, "expected '(' after table name")) return nullptr;
  if (!Expect(TokenType::IDENTIFIER, "expected column name in index")) return nullptr;
  stmt->column_name = Previous().lexeme;
  if (!Expect(TokenType::RPAREN, "expected ')' after indexed column")) return nullptr;

  return stmt;
}

std::unique_ptr<CreateTableStmt> Parser::ParseCreateTable() {
  if (!Expect(TokenType::TABLE, "expected 'TABLE' after 'CREATE'")) return nullptr;
  if (!Expect(TokenType::IDENTIFIER, "expected table name")) return nullptr;
  std::string table_name = Previous().lexeme;

  if (!Expect(TokenType::LPAREN, "expected '(' after table name")) return nullptr;

  auto stmt = std::make_unique<CreateTableStmt>();
  stmt->table_name = table_name;

  while (!Check(TokenType::RPAREN) && !IsAtEnd()) {
    // Check if table constraint
    if (Match(TokenType::PRIMARY)) {
      if (!Expect(TokenType::KEY, "expected 'KEY' after 'PRIMARY'")) return nullptr;
      if (!Expect(TokenType::LPAREN, "expected '(' after 'PRIMARY KEY'")) return nullptr;
      while (!Check(TokenType::RPAREN) && !IsAtEnd()) {
        if (!Expect(TokenType::IDENTIFIER, "expected column name in primary key")) return nullptr;
        stmt->primary_key_columns.push_back(Previous().lexeme);
        if (!Match(TokenType::COMMA)) break;
      }
      if (!Expect(TokenType::RPAREN, "expected ')' after primary key columns")) return nullptr;
    } else if (Match(TokenType::UNIQUE)) {
      if (!Expect(TokenType::LPAREN, "expected '(' after 'UNIQUE'")) return nullptr;
      std::vector<std::string> u_cols;
      while (!Check(TokenType::RPAREN) && !IsAtEnd()) {
        if (!Expect(TokenType::IDENTIFIER, "expected column name in unique constraint")) return nullptr;
        u_cols.push_back(Previous().lexeme);
        if (!Match(TokenType::COMMA)) break;
      }
      if (!Expect(TokenType::RPAREN, "expected ')' after unique constraint columns")) return nullptr;
      stmt->unique_constraints.push_back(u_cols);
    } else if (Match(TokenType::CHECK)) {
      if (!Expect(TokenType::LPAREN, "expected '(' after 'CHECK'")) return nullptr;
      size_t depth = 1;
      std::string chk_str;
      while (depth > 0 && !IsAtEnd()) {
        if (Check(TokenType::LPAREN)) ++depth;
        else if (Check(TokenType::RPAREN)) {
          --depth;
          if (depth == 0) {
            Advance();
            break;
          }
        }
        chk_str += Peek().lexeme + " ";
        Advance();
      }
      CheckConstraint chk;
      chk.expression = chk_str;
      stmt->check_constraints.push_back(chk);
    } else if (Match(TokenType::FOREIGN)) {
      if (!Expect(TokenType::KEY, "expected 'KEY' after 'FOREIGN'")) return nullptr;
      if (!Expect(TokenType::LPAREN, "expected '(' after 'FOREIGN KEY'")) return nullptr;
      ForeignKey fk;
      while (!Check(TokenType::RPAREN) && !IsAtEnd()) {
        if (!Expect(TokenType::IDENTIFIER, "expected column name in foreign key")) return nullptr;
        fk.column_names.push_back(Previous().lexeme);
        if (!Match(TokenType::COMMA)) break;
      }
      if (!Expect(TokenType::RPAREN, "expected ')' after foreign key columns")) return nullptr;
      if (!Expect(TokenType::REFERENCES, "expected 'REFERENCES'")) return nullptr;
      if (!Expect(TokenType::IDENTIFIER, "expected referenced table name")) return nullptr;
      fk.ref_table_name = Previous().lexeme;
      if (!Expect(TokenType::LPAREN, "expected '(' after referenced table name")) return nullptr;
      while (!Check(TokenType::RPAREN) && !IsAtEnd()) {
        if (!Expect(TokenType::IDENTIFIER, "expected referenced column name")) return nullptr;
        fk.ref_column_names.push_back(Previous().lexeme);
        if (!Match(TokenType::COMMA)) break;
      }
      if (!Expect(TokenType::RPAREN, "expected ')' after referenced columns")) return nullptr;

      if (Match(TokenType::ON)) {
        if (!Expect(TokenType::DELETE, "expected 'DELETE' after 'ON'")) return nullptr;
        if (Match(TokenType::CASCADE)) {
          fk.on_delete = ForeignKey::OnDelete::CASCADE;
        } else if (Match(TokenType::RESTRICT)) {
          fk.on_delete = ForeignKey::OnDelete::RESTRICT;
        } else {
          SetError("expected 'RESTRICT' or 'CASCADE' after 'ON DELETE'", Peek());
          return nullptr;
        }
      }
      stmt->foreign_keys.push_back(fk);
    } else {
      // Column definition: <name> <type> [constraints...]
      if (!Expect(TokenType::IDENTIFIER, "expected column name or constraint")) return nullptr;
      Column col;
      col.name = Previous().lexeme;

      // Type
      if (Match(TokenType::INT) || Match(TokenType::INTEGER)) {
        col.type = DataType::INT;
        col.length = 4;
      } else if (Match(TokenType::VARCHAR)) {
        col.type = DataType::VARCHAR;
        if (!Expect(TokenType::LPAREN, "expected '(' after VARCHAR")) return nullptr;
        if (!Expect(TokenType::INTEGER_LITERAL, "expected integer length for VARCHAR")) return nullptr;
        col.length = static_cast<uint32_t>(std::stoul(Previous().lexeme));
        if (!Expect(TokenType::RPAREN, "expected ')' after VARCHAR length")) return nullptr;
      } else if (Match(TokenType::DECIMAL)) {
        col.type = DataType::DECIMAL;
        col.length = 8;
        if (Match(TokenType::LPAREN)) {
          // Optional precision, scale: DECIMAL(10, 2)
          Expect(TokenType::INTEGER_LITERAL, "expected precision");
          if (Match(TokenType::COMMA)) {
            Expect(TokenType::INTEGER_LITERAL, "expected scale");
          }
          Expect(TokenType::RPAREN, "expected ')'");
        }
      } else {
        SetError("expected data type (INT, VARCHAR, DECIMAL)", Peek());
        return nullptr;
      }

      // Column constraints
      while (!Check(TokenType::COMMA) && !Check(TokenType::RPAREN) && !IsAtEnd()) {
        if (Match(TokenType::NOT)) {
          if (!Expect(TokenType::NULL_KW, "expected 'NULL' after 'NOT'")) return nullptr;
          col.not_null = true;
        } else if (Match(TokenType::PRIMARY)) {
          if (!Expect(TokenType::KEY, "expected 'KEY' after 'PRIMARY'")) return nullptr;
          col.primary_key = true;
          stmt->primary_key_columns.push_back(col.name);
        } else if (Match(TokenType::UNIQUE)) {
          col.unique = true;
          stmt->unique_constraints.push_back({col.name});
        } else if (Match(TokenType::CHECK)) {
          if (!Expect(TokenType::LPAREN, "expected '(' after 'CHECK'")) return nullptr;
          size_t depth = 1;
          std::string chk_str;
          while (depth > 0 && !IsAtEnd()) {
            if (Check(TokenType::LPAREN)) ++depth;
            else if (Check(TokenType::RPAREN)) {
              --depth;
              if (depth == 0) {
                Advance();
                break;
              }
            }
            chk_str += Peek().lexeme + " ";
            Advance();
          }
          CheckConstraint chk;
          chk.expression = chk_str;
          stmt->check_constraints.push_back(chk);
        } else if (Match(TokenType::REFERENCES)) {
          ForeignKey fk;
          fk.column_names.push_back(col.name);
          if (!Expect(TokenType::IDENTIFIER, "expected referenced table")) return nullptr;
          fk.ref_table_name = Previous().lexeme;
          if (!Expect(TokenType::LPAREN, "expected '('")) return nullptr;
          if (!Expect(TokenType::IDENTIFIER, "expected referenced column")) return nullptr;
          fk.ref_column_names.push_back(Previous().lexeme);
          if (!Expect(TokenType::RPAREN, "expected ')'")) return nullptr;
          if (Match(TokenType::ON)) {
            if (!Expect(TokenType::DELETE, "expected 'DELETE' after 'ON'")) return nullptr;
            if (Match(TokenType::CASCADE)) fk.on_delete = ForeignKey::OnDelete::CASCADE;
            else if (Match(TokenType::RESTRICT)) fk.on_delete = ForeignKey::OnDelete::RESTRICT;
          }
          stmt->foreign_keys.push_back(fk);
        } else {
          break;
        }
      }

      stmt->columns.push_back(col);
    }

    if (!Match(TokenType::COMMA)) break;
  }

  if (!Expect(TokenType::RPAREN, "expected ')' at end of column list")) return nullptr;
  Match(TokenType::SEMICOLON);
  return stmt;
}

std::unique_ptr<InsertStmt> Parser::ParseInsert() {
  if (!Expect(TokenType::INTO, "expected 'INTO' after 'INSERT'")) return nullptr;
  if (!Expect(TokenType::IDENTIFIER, "expected table name")) return nullptr;
  auto stmt = std::make_unique<InsertStmt>();
  stmt->table_name = Previous().lexeme;

  // Optional column list: (col1, col2, ...)
  if (Match(TokenType::LPAREN)) {
    while (!Check(TokenType::RPAREN) && !IsAtEnd()) {
      if (!Expect(TokenType::IDENTIFIER, "expected column name")) return nullptr;
      stmt->column_names.push_back(Previous().lexeme);
      if (!Match(TokenType::COMMA)) break;
    }
    if (!Expect(TokenType::RPAREN, "expected ')' after column list")) return nullptr;
  }

  if (!Expect(TokenType::VALUES, "expected 'VALUES'")) return nullptr;

  while (!IsAtEnd()) {
    if (!Expect(TokenType::LPAREN, "expected '(' before VALUES list")) return nullptr;
    std::vector<std::unique_ptr<Expr>> row_vals;
    while (!Check(TokenType::RPAREN) && !IsAtEnd()) {
      auto expr = ParseExpression();
      if (!expr) return nullptr;
      row_vals.push_back(std::move(expr));
      if (!Match(TokenType::COMMA)) break;
    }
    if (!Expect(TokenType::RPAREN, "expected ')' after VALUES list")) return nullptr;
    stmt->values_matrix.push_back(std::move(row_vals));

    if (!Match(TokenType::COMMA)) break;
  }

  Match(TokenType::SEMICOLON);
  return stmt;
}

bool Parser::CheckInnerJoin() const {
  if (!Check(TokenType::IDENTIFIER) || Peek().lexeme != "inner") return false;
  const size_t next = current_ + 1;
  return next < tokens_.size() && tokens_[next].type == TokenType::JOIN;
}

std::unique_ptr<SelectStmt> Parser::ParseSelect() {
  auto stmt = std::make_unique<SelectStmt>();

  // Select list
  if (Match(TokenType::STAR)) {
    stmt->is_select_all = true;
    SelectItem item;
    item.is_star = true;
    stmt->select_list.push_back(std::move(item));
  } else {
    while (!IsAtEnd()) {
      SelectItem item;
      auto expr = ParseExpression();
      if (!expr) return nullptr;
      item.expr = std::move(expr);
      if (Match(TokenType::AS)) {
        if (!Expect(TokenType::IDENTIFIER, "expected alias name after 'AS'")) return nullptr;
        item.alias = Previous().lexeme;
      } else if (Check(TokenType::IDENTIFIER) && !Check(TokenType::FROM)) {
        Advance();
        item.alias = Previous().lexeme;
      }
      stmt->select_list.push_back(std::move(item));
      if (!Match(TokenType::COMMA)) break;
    }
  }

  if (!Expect(TokenType::FROM, "expected 'FROM' clause")) return nullptr;
  if (!Expect(TokenType::IDENTIFIER, "expected table name after 'FROM'")) return nullptr;
  stmt->from_table = Previous().lexeme;

  // Optional JOIN clauses, chained: `JOIN t ON p (JOIN t2 ON p2)*`.
  while (true) {
    bool matched = Match(TokenType::JOIN);
    if (!matched && CheckInnerJoin()) {
      Advance();  // 'inner'
      Advance();  // 'JOIN'
      matched = true;
    }
    if (!matched) break;

    JoinClause clause;
    if (!Expect(TokenType::IDENTIFIER, "expected table name after 'JOIN'")) return nullptr;
    clause.table_name = Previous().lexeme;
    if (!Expect(TokenType::ON, "expected 'ON' after joined table")) return nullptr;
    clause.on = ParseExpression();
    if (!clause.on) return nullptr;
    stmt->joins.push_back(std::move(clause));
  }

  // Optional WHERE
  if (Match(TokenType::WHERE)) {
    stmt->where_clause = ParseExpression();
    if (!stmt->where_clause) return nullptr;
  }

  // Optional GROUP BY
  if (Match(TokenType::GROUP)) {
    if (!Expect(TokenType::BY, "expected 'BY' after 'GROUP'")) return nullptr;
    while (!IsAtEnd()) {
      if (!Expect(TokenType::IDENTIFIER, "expected column name in GROUP BY")) return nullptr;
      std::string col = Previous().lexeme;
      if (Match(TokenType::DOT)) {
        if (!Expect(TokenType::IDENTIFIER, "expected column name after '.'")) return nullptr;
        col += "." + Previous().lexeme;
      }
      stmt->group_by_columns.push_back(col);
      if (!Match(TokenType::COMMA)) break;
    }
  }

  // Optional ORDER BY
  if (Match(TokenType::ORDER)) {
    if (!Expect(TokenType::BY, "expected 'BY' after 'ORDER'")) return nullptr;
    while (!IsAtEnd()) {
      if (!Expect(TokenType::IDENTIFIER, "expected column name in ORDER BY")) return nullptr;
      OrderByItem item;
      std::string col = Previous().lexeme;
      if (Match(TokenType::DOT)) {
        if (!Expect(TokenType::IDENTIFIER, "expected column name after '.'")) return nullptr;
        col += "." + Previous().lexeme;
      }
      item.column_name = col;
      if (Match(TokenType::DESC)) {
        item.is_desc = true;
      } else {
        Match(TokenType::ASC);
        item.is_desc = false;
      }
      stmt->order_by.push_back(item);
      if (!Match(TokenType::COMMA)) break;
    }
  }

  Match(TokenType::SEMICOLON);
  return stmt;
}

std::unique_ptr<UpdateStmt> Parser::ParseUpdate() {
  if (!Expect(TokenType::IDENTIFIER, "expected table name after 'UPDATE'")) return nullptr;
  auto stmt = std::make_unique<UpdateStmt>();
  stmt->table_name = Previous().lexeme;

  if (!Expect(TokenType::SET, "expected 'SET' in UPDATE statement")) return nullptr;

  while (!IsAtEnd()) {
    if (!Expect(TokenType::IDENTIFIER, "expected column name in assignment")) return nullptr;
    std::string col = Previous().lexeme;
    if (!Expect(TokenType::EQ, "expected '=' in assignment")) return nullptr;
    auto val_expr = ParseExpression();
    if (!val_expr) return nullptr;
    stmt->assignments.push_back({col, std::move(val_expr)});
    if (!Match(TokenType::COMMA)) break;
  }

  if (Match(TokenType::WHERE)) {
    stmt->where_clause = ParseExpression();
    if (!stmt->where_clause) return nullptr;
  }

  Match(TokenType::SEMICOLON);
  return stmt;
}

std::unique_ptr<DeleteStmt> Parser::ParseDelete() {
  if (!Expect(TokenType::FROM, "expected 'FROM' after 'DELETE'")) return nullptr;
  if (!Expect(TokenType::IDENTIFIER, "expected table name after 'DELETE FROM'")) return nullptr;
  auto stmt = std::make_unique<DeleteStmt>();
  stmt->table_name = Previous().lexeme;

  if (Match(TokenType::WHERE)) {
    stmt->where_clause = ParseExpression();
    if (!stmt->where_clause) return nullptr;
  }

  Match(TokenType::SEMICOLON);
  return stmt;
}

std::unique_ptr<TransactionStmt> Parser::ParseTransaction() {
  TransactionStmt::Action act;
  if (Match(TokenType::BEGIN_KW)) act = TransactionStmt::Action::BEGIN;
  else if (Match(TokenType::COMMIT)) act = TransactionStmt::Action::COMMIT;
  else if (Match(TokenType::ABORT) || Match(TokenType::ROLLBACK)) act = TransactionStmt::Action::ROLLBACK;
  else if (Match(TokenType::CHECKPOINT)) act = TransactionStmt::Action::CHECKPOINT;
  else {
    SetError("expected transaction statement", Peek());
    return nullptr;
  }
  Match(TokenType::TRANSACTION);
  Match(TokenType::SEMICOLON);
  return std::make_unique<TransactionStmt>(act);
}

std::unique_ptr<AnalyzeStmt> Parser::ParseAnalyze() {
  if (!Expect(TokenType::IDENTIFIER, "expected table name after 'ANALYZE'")) return nullptr;
  auto stmt = std::make_unique<AnalyzeStmt>(Previous().lexeme);
  Match(TokenType::SEMICOLON);
  return stmt;
}

std::unique_ptr<SetStmt> Parser::ParseSet() {
  if (!Expect(TokenType::IDENTIFIER, "expected variable name after 'SET'")) return nullptr;
  std::string flag = Previous().lexeme;
  if (!Expect(TokenType::EQ, "expected '=' after variable name")) return nullptr;

  if (IsAtEnd() || Check(TokenType::SEMICOLON)) {
    SetError("expected value after '='", Peek());
    return nullptr;
  }
  std::string val = Advance().lexeme;

  Match(TokenType::SEMICOLON);
  return std::make_unique<SetStmt>(flag, val);
}

std::unique_ptr<ExplainStmt> Parser::ParseExplain() {
  auto inner = ParseStatement();
  if (!inner) return nullptr;
  return std::make_unique<ExplainStmt>(std::move(inner));
}

// -------------------------------------------------------------
// Expressions
// -------------------------------------------------------------

std::unique_ptr<Expr> Parser::ParseExpression() {
  return ParseOr();
}

std::unique_ptr<Expr> Parser::ParseOr() {
  auto expr = ParseAnd();
  if (!expr) return nullptr;

  while (Match(TokenType::OR)) {
    auto right = ParseAnd();
    if (!right) return nullptr;
    expr = std::make_unique<BinaryExpr>(BinaryOp::OR, std::move(expr), std::move(right));
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseAnd() {
  auto expr = ParseNot();
  if (!expr) return nullptr;

  while (Match(TokenType::AND)) {
    auto right = ParseNot();
    if (!right) return nullptr;
    expr = std::make_unique<BinaryExpr>(BinaryOp::AND, std::move(expr), std::move(right));
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseNot() {
  if (Match(TokenType::NOT)) {
    auto child = ParseNot();
    if (!child) return nullptr;
    return std::make_unique<UnaryExpr>(UnaryOp::NOT, std::move(child));
  }
  return ParseComparison();
}

std::unique_ptr<Expr> Parser::ParseComparison() {
  auto expr = ParseTerm();
  if (!expr) return nullptr;

  if (Match(TokenType::EQ)) {
    auto right = ParseTerm();
    if (!right) return nullptr;
    return std::make_unique<BinaryExpr>(BinaryOp::EQ, std::move(expr), std::move(right));
  }
  if (Match(TokenType::NEQ)) {
    auto right = ParseTerm();
    if (!right) return nullptr;
    return std::make_unique<BinaryExpr>(BinaryOp::NEQ, std::move(expr), std::move(right));
  }
  if (Match(TokenType::LT)) {
    auto right = ParseTerm();
    if (!right) return nullptr;
    return std::make_unique<BinaryExpr>(BinaryOp::LT, std::move(expr), std::move(right));
  }
  if (Match(TokenType::LTE)) {
    auto right = ParseTerm();
    if (!right) return nullptr;
    return std::make_unique<BinaryExpr>(BinaryOp::LTE, std::move(expr), std::move(right));
  }
  if (Match(TokenType::GT)) {
    auto right = ParseTerm();
    if (!right) return nullptr;
    return std::make_unique<BinaryExpr>(BinaryOp::GT, std::move(expr), std::move(right));
  }
  if (Match(TokenType::GTE)) {
    auto right = ParseTerm();
    if (!right) return nullptr;
    return std::make_unique<BinaryExpr>(BinaryOp::GTE, std::move(expr), std::move(right));
  }

  // IS [NOT] NULL
  if (Match(TokenType::IS)) {
    bool is_not = false;
    if (Match(TokenType::NOT)) {
      is_not = true;
    }
    if (!Expect(TokenType::NULL_KW, "expected 'NULL' after 'IS'")) return nullptr;
    return std::make_unique<IsNullExpr>(std::move(expr), is_not);
  }

  return expr;
}

std::unique_ptr<Expr> Parser::ParseTerm() {
  auto expr = ParseFactor();
  if (!expr) return nullptr;

  while (Match(TokenType::PLUS) || Match(TokenType::MINUS)) {
    TokenType op_tok = Previous().type;
    auto right = ParseFactor();
    if (!right) return nullptr;
    expr = std::make_unique<BinaryExpr>(
        op_tok == TokenType::PLUS ? BinaryOp::ADD : BinaryOp::SUB,
        std::move(expr), std::move(right));
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseFactor() {
  auto expr = ParseUnary();
  if (!expr) return nullptr;

  while (Match(TokenType::STAR) || Match(TokenType::SLASH)) {
    TokenType op_tok = Previous().type;
    auto right = ParseUnary();
    if (!right) return nullptr;
    expr = std::make_unique<BinaryExpr>(
        op_tok == TokenType::STAR ? BinaryOp::MUL : BinaryOp::DIV,
        std::move(expr), std::move(right));
  }
  return expr;
}

std::unique_ptr<Expr> Parser::ParseUnary() {
  if (Match(TokenType::MINUS)) {
    auto child = ParseUnary();
    if (!child) return nullptr;
    return std::make_unique<UnaryExpr>(UnaryOp::NEGATE, std::move(child));
  }
  return ParsePrimary();
}

std::unique_ptr<Expr> Parser::ParsePrimary() {
  if (Match(TokenType::INTEGER_LITERAL)) {
    int64_t val = std::stoll(Previous().lexeme);
    return LiteralExpr::MakeInt(val);
  }
  if (Match(TokenType::DECIMAL_LITERAL)) {
    double val = std::stod(Previous().lexeme);
    return LiteralExpr::MakeDecimal(val);
  }
  if (Match(TokenType::STRING_LITERAL)) {
    return LiteralExpr::MakeString(Previous().lexeme);
  }
  if (Match(TokenType::NULL_KW)) {
    return LiteralExpr::MakeNull();
  }

  // Aggregate functions: COUNT, SUM, MIN, MAX, AVG
  if (Match(TokenType::COUNT)) {
    if (!Expect(TokenType::LPAREN, "expected '(' after 'COUNT'")) return nullptr;
    if (Match(TokenType::STAR)) {
      if (!Expect(TokenType::RPAREN, "expected ')' after '*'")) return nullptr;
      return std::make_unique<AggregateExpr>(AggFunc::COUNT, true, nullptr);
    }
    auto arg = ParseExpression();
    if (!arg) return nullptr;
    if (!Expect(TokenType::RPAREN, "expected ')' after COUNT argument")) return nullptr;
    return std::make_unique<AggregateExpr>(AggFunc::COUNT, false, std::move(arg));
  }
  if (Match(TokenType::SUM) || Match(TokenType::MIN) || Match(TokenType::MAX) || Match(TokenType::AVG)) {
    TokenType func_tok = Previous().type;
    AggFunc func = AggFunc::SUM;
    if (func_tok == TokenType::MIN) func = AggFunc::MIN;
    else if (func_tok == TokenType::MAX) func = AggFunc::MAX;
    else if (func_tok == TokenType::AVG) func = AggFunc::AVG;

    if (!Expect(TokenType::LPAREN, "expected '(' after aggregate function")) return nullptr;
    auto arg = ParseExpression();
    if (!arg) return nullptr;
    if (!Expect(TokenType::RPAREN, "expected ')' after aggregate argument")) return nullptr;
    return std::make_unique<AggregateExpr>(func, false, std::move(arg));
  }

  // Column reference: [table.]column
  if (Match(TokenType::IDENTIFIER)) {
    std::string first = Previous().lexeme;
    if (Match(TokenType::DOT)) {
      if (!Expect(TokenType::IDENTIFIER, "expected column name after '.'")) return nullptr;
      std::string col = Previous().lexeme;
      return std::make_unique<ColumnRefExpr>(first, col);
    }
    return std::make_unique<ColumnRefExpr>("", first);
  }

  // Parenthesized expression: ( expr )
  if (Match(TokenType::LPAREN)) {
    auto expr = ParseExpression();
    if (!expr) return nullptr;
    if (!Expect(TokenType::RPAREN, "expected ')' after parenthesized expression")) return nullptr;
    return expr;
  }

  SetError("expected expression", Peek());
  return nullptr;
}

}  // namespace sthir
