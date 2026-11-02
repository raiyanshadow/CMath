#include "LanguageInternal.h"

namespace cmath::language::detail {
    Parser::Parser(const std::vector<Token>& tokens, std::size_t maximumDiagnostics)
    : tokens_(tokens), maximumDiagnostics_(maximumDiagnostics) {}

    const Token& Parser::peek(std::size_t lookahead) const {
        const auto index = current_ + lookahead;
        return tokens_[index < tokens_.size() ? index : tokens_.size() - 1];
    }

    bool Parser::check(TokenKind kind) const { return peek().kind == kind; }
    bool Parser::match(TokenKind kind) {
        if (!check(kind)) return false;
        advance();
        return true;
    } 

    const Token& Parser::advance() {
        if (!check(TokenKind::EndOfFile)) ++current_;
        return tokens_[current_ - 1];
    }

    bool Parser::consume(TokenKind kind, const std::string& message) {
        if (check(kind)) {advance(); return true;}
        error(peek(), "ParseError", message);
        return false;
    }

    void Parser::error(const Token& token, std::string code, std::string message) {
        if (diagnostics_.size() >= maximumDiagnostics_) return;
        diagnostics_.push_back(Diagnostic{DiagnosticSeverity::Error, std::move(code), std::move(message), token.location, {}});
    }

    void Parser::error(const SourceLocation& location, std::string code,std::string message) {
        if (diagnostics_.size() >= maximumDiagnostics_) return;

        diagnostics_.push_back(Diagnostic{
            DiagnosticSeverity::Error,
            std::move(code),
            std::move(message),
            location,
            {}
        });
    }

    void Parser::synchronize() {
        while (!check(TokenKind::EndOfFile)) {
            if (check(TokenKind::Newline)) {advance(); return;}
            if (check(TokenKind::Dedent)) return;
            switch (peek().kind) {
                case TokenKind::Let:
                case TokenKind::Var:
                case TokenKind::Def:
                case TokenKind::Return:
                case TokenKind::If:
                case TokenKind::While:
                case TokenKind::For:
                case TokenKind::Break:
                case TokenKind::Continue:
                    return;
                default:
                    advance();
                    break;
            }
        }
    }

    ParseResult Parser::parse() {
        AstProgram program;
        while (match(TokenKind::Newline)) {}
        if (!consume(TokenKind::Domain, "Every CMath file must begin with a domain declaration")) {
            return ParseResult{std::nullopt, std::move(diagnostics_)};
        }
        const Token domainToken = peek();
        if (!consume(TokenKind::Identifier, "Expected a domain name after 'domain' keyword")) {
            return ParseResult{std::nullopt, std::move(diagnostics_)};
        }
        program.defaultDomain = domainToken.lexeme;
        program.domainLocation = domainToken.location;
        program.body.location = domainToken.location;

        if (check(TokenKind::Newline)) advance();
        else if (!check(TokenKind::EndOfFile)) error(peek(), "ParseError", "Expected newline after domain declaration");

        while (!check(TokenKind::EndOfFile)) {
            if (match(TokenKind::Newline)) continue;
            if (check(TokenKind::Dedent) || check(TokenKind::Indent)) {
                error(peek(), "ParseError", "Unexpected indentation at top level");
                advance();
                continue;
            }
            const auto before = current_;
            program.body.statements.push_back(parseStatement());
            if (current_ == before) advance();
        }

        if (!diagnostics_.empty()) return ParseResult{std::nullopt, std::move(diagnostics_)};
        return ParseResult{std::move(program), std::move(diagnostics_)};
    }

    Statement Parser::parseStatement() {
        if (check(TokenKind::Let) || check(TokenKind::Var)) return parseVariable();
        if (check(TokenKind::Def)) return parseFunction();
        if (check(TokenKind::Return)) return parseReturn();
        if (check(TokenKind::If)) return parseIf();
        if (check(TokenKind::While)) return parseWhile();
        if (check(TokenKind::For)) return parseFor();
        if (check(TokenKind::Break) || check(TokenKind::Continue)) return parseLoopControl();
        return parseAssignmentOrExpression();
    }

    Statement Parser::parseVariable() {
        const Token keyword = advance();
        const Token name = peek();
        if (!consume(TokenKind::Identifier, "Expected variable name")) synchronize();
        Statement statement;
        statement.kind = Statement::Kind::Variable;
        statement.location = keyword.location;
        statement.name = name.lexeme;
        statement.mutableBinding = keyword.kind == TokenKind::Var;
        if (match(TokenKind::Colon)) {
            const Token type = peek();
            if (consume(TokenKind::Identifier, "Expected a type name after ':'"))
                statement.typeName = type.lexeme;
        }
        if (!consume(TokenKind::Equal, "Expected '=' in variable declaration")) synchronize();
        statement.expression = parseExpression();
        if (!check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) 
            error(peek(), "ParseError", "Unexpected token after variable initializer");
        match(TokenKind::Newline);
        return statement;
    }

    Statement Parser::parseFunction() {
        const Token keyword = advance();
        
        Statement statement;
        statement.kind = Statement::Kind::Function;
        statement.location = keyword.location;
        
        const Token name = peek();
        
        if (!consume(TokenKind::Identifier, "Expected a function name after 'def'")) {
            synchronize();
            return statement;
        }

        statement.name = name.lexeme;

        if (!consume(TokenKind::LeftParen, "Expected '(' after function name")) {
            synchronize();
            return statement;
        }

        if (!check(TokenKind::RightParen)) {
            do {
                const Token parameterName = peek();
                if (!consume(TokenKind::Identifier, "Expected parameter name")) {
                    synchronize();
                    return statement;
                }
                if (!consume(TokenKind::Colon, "Function parameters require type annotations")) {
                    synchronize();
                    return statement;
                }
                const Token type = peek();
                if (!consume(TokenKind::Identifier, "Expected parameter type")) {
                    synchronize();
                    return statement;
                }
                statement.parameters.push_back(ParameterAst{parameterName.lexeme, type.lexeme, parameterName.location});
            } while (match(TokenKind::Comma));
        }

        if(!consume(TokenKind::RightParen, "Expected ')' after function parameters")) {
            synchronize();
            return statement;
        }
        
        if (match(TokenKind::Arrow)) {
            const Token resultType = peek();
            if (!consume(TokenKind::Identifier, "Expected return type after '->'")) {
                synchronize();
                return statement;
            }
            statement.returnType = resultType.lexeme;
        } else {
            error(peek(), "ParseError", "Function return types are required");
        }
        if (!consume(TokenKind::Colon, "Expected ':' after function signature")) {
            synchronize();
            return statement;
        }

        statement.body = parseIndentedBlock();
        return statement;
    }

    std::shared_ptr<AstBlock> Parser::parseIndentedBlock() {
        auto block = std::make_shared<AstBlock>();
        block->location = peek().location;
        if (!consume(TokenKind::Newline, "Expected a newline before the block body")) {
            if (!check(TokenKind::Indent)) return block;
        }
        if (!consume(TokenKind::Indent, "Expected an indented function body")) return block;
        while (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
            if (match(TokenKind::Newline)) continue;
            if (check(TokenKind::Indent)) {
                error(peek(), "ParseError", "Unexpected indentation inside block");
                advance();
                continue;
            }
            const auto before = current_;
            block->statements.push_back(parseStatement());
            if (current_ == before) advance();
        }
        if (!consume(TokenKind::Dedent, "Expected the end of the indented block")) {
            return block;
        }
        return block;
    }

    Statement Parser::parseReturn() {
        const Token keyword = advance();
        Statement statement;
        statement.kind = Statement::Kind::Return;
        statement.location = keyword.location;
        if (!check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) 
            statement.expression = parseExpression();
        if (!check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
            error(peek(), "ParseError", "Unexpected token after return expression");
            synchronize();
        }
        match(TokenKind::Newline);
        return statement;
    }

    Statement Parser::parseIf() {
        const Token keyword = advance();
        Statement statement;
        statement.kind = Statement::Kind::If;
        statement.location = keyword.location;

        auto parseBranch = 
            [this](const Token& branchKeyword) -> AstBranch {
                AstBranch branch;

                branch.location = branchKeyword.location;
                branch.condition = parseExpression();

                if (!consume(TokenKind::Colon, "Expected ':' after conditional expression")) {
                    return branch;
                }

                branch.body = parseIndentedBlock();
                return branch;
            };
        
        statement.branches.push_back(parseBranch(keyword));

        while (true) {
            if (match(TokenKind::Elif)) {
                const Token branchKeyword = tokens_[current_ - 1];
                statement.branches.push_back(parseBranch(branchKeyword));
                continue;
            }

            if (check(TokenKind::Else) && peek(1).kind == TokenKind::If) {
                advance();
                const Token branchKeyword = advance();
                statement.branches.push_back(parseBranch(branchKeyword));
                continue;
            }

            break;
        }

        if (match(TokenKind::Else)) {
            const Token elseKeyword = tokens_[current_ - 1];

            if (!consume(TokenKind::Colon, "Expected ':' after 'else'")) {
                return statement;
            }

            statement.elseBody = parseIndentedBlock();
            statement.elseBody->location = elseKeyword.location;
        }

        return statement;
    }

    Statement Parser::parseWhile() {
        const Token keyword = advance();

        Statement statement;
        statement.kind = Statement::Kind::While;
        statement.location = keyword.location;

        statement.expression = parseExpression();

        if (!consume(TokenKind::Colon, "Expected ':' after while condition")) {
            return statement;
        }

        statement.body = parseIndentedBlock();
        return statement;
    }

    Statement Parser::parseFor() {
        const Token keyword = advance();

        Statement statement;
        statement.kind = Statement::Kind::For;
        statement.location = keyword.location;

        const Token iterator = peek();

        if (!consume(TokenKind::Identifier, "Expected an iterator name after 'for'")) {
            synchronize();
            return statement;
        }

        statement.name = iterator.lexeme;

        if (!consume(TokenKind::In, "Expected 'in' after for-loop iterator")) {
            synchronize();
            return statement;
        }

        statement.expression = parseExpression();

        if (statement.expression && statement.expression->kind != Expr::Kind::Range) {
            error(statement.expression->location, "ParseError", "A for loop requires a range expression of the form start..end");
        }

        if (!consume(TokenKind::Colon, "Expected ':' after for-loop range")) {
            return statement;
        }
        statement.body = parseIndentedBlock();
        return statement;
    }

    Statement Parser::parseLoopControl() {
        const Token keyword = advance();

        Statement statement;
        statement.location = keyword.location;

        if (keyword.kind == TokenKind::Break) {
            statement.kind = Statement::Kind::Break;
        } else {
            statement.kind = Statement::Kind::Continue;
        }

        if (
            !check(TokenKind::Newline) && 
            !check(TokenKind::Dedent) &&
            !check(TokenKind::EndOfFile)
        ) {
            error(peek(), "ParseError", "Unexpected expression after loop-control statement");
            synchronize();
        }

        match(TokenKind::Newline);
        return statement;
    }

    Statement Parser::parseAssignmentOrExpression() {
        auto isAssignmentOperator = [](TokenKind kind) {
            return kind == TokenKind::Equal ||
                kind == TokenKind::PlusEqual ||
                kind == TokenKind::MinusEqual ||
                kind == TokenKind::StarEqual ||
                kind == TokenKind::SlashEqual ||
                kind == TokenKind::PercentEqual ||
                kind == TokenKind::CaretEqual;
        };

        if (check(TokenKind::Identifier) &&
            isAssignmentOperator(peek(1).kind)) {

            const Token name = advance();
            const Token op = advance();

            Statement statement;
            statement.kind = Statement::Kind::Assignment;
            statement.location = name.location;
            statement.name = name.lexeme;
            statement.assignmentOperator = op.lexeme;
            statement.expression = parseExpression();

            if (!check(TokenKind::Newline) &&
                !check(TokenKind::Dedent) &&
                !check(TokenKind::EndOfFile)) {
                error(
                    peek(),
                    "ParseError",
                    "Unexpected token after assignment expression"
                );
                synchronize();
            }

            match(TokenKind::Newline);
            return statement;
        }

        Statement statement;
        statement.kind = Statement::Kind::Expression;
        statement.location = peek().location;
        statement.expression = parseExpression();

        if (!check(TokenKind::Newline) &&
            !check(TokenKind::Dedent) &&
            !check(TokenKind::EndOfFile)) {
            error(
                peek(),
                "ParseError",
                "Unexpected token after expression"
            );
            synchronize();
        }

        match(TokenKind::Newline);
        return statement;
    }

    ExprPtr Parser::makeExpr(Expr::Kind kind, std::string text, SourceLocation location, std::vector<ExprPtr> children) {
        auto expression = std::make_shared<Expr>();
        expression->kind = kind;
        expression->text = std::move(text);
        expression->location = location;
        expression->children = std::move(children);
        return expression;
    }

    ExprPtr Parser::parseExpression() {return parseOr();}

    ExprPtr Parser::parseOr() {
        auto expression = parseAnd();
        while (match(TokenKind::Or)) {
            const Token op = tokens_[current_ - 1];
            auto right = parseAnd();
            expression = makeExpr(Expr::Kind::Binary, op.lexeme, op.location, {expression, right});
        }
        return expression;
    }

    ExprPtr Parser::parseAnd() {
        auto expression = parseNot();
        while (match(TokenKind::And)) {
            const Token op = tokens_[current_ - 1];
            auto right = parseNot();
            expression = makeExpr(Expr::Kind::Binary, op.lexeme, op.location, {expression, right});
        }
        return expression;
    }

    ExprPtr Parser::parseNot() {
        if (match(TokenKind::Not)) {
            const Token op = tokens_[current_ - 1];
            return makeExpr(Expr::Kind::Unary, op.lexeme, op.location, {parseNot()});
        }
        return parseComparison();
    }

    ExprPtr Parser::parseComparison() {
        auto expression = parseRange();
        while (check(TokenKind::EqualEqual) || check(TokenKind::NotEqual) ||
               check(TokenKind::Less) || check(TokenKind::Greater) ||
               check(TokenKind::LessEqual) || check(TokenKind::GreaterEqual)) {
            const Token op = advance();
            auto right = parseRange();
            expression = makeExpr(Expr::Kind::Binary, op.lexeme, op.location, {expression, right});
        }
        return expression;
    }

    ExprPtr Parser::parseRange() {
        auto expression = parseAdditive();
        if (match(TokenKind::DotDot)) {
            const Token op = tokens_[current_ - 1];
            auto right = parseAdditive();
            expression = makeExpr(Expr::Kind::Range, op.lexeme, op.location, {expression, right});
            if (check(TokenKind::DotDot)) {
                error(peek(), "ParseError", "Chained range expressions are not supported");
            }
        }
        return expression;
    }

    ExprPtr Parser::parseAdditive() {
        auto expression = parseMultiplicative();
        while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
            const Token op = advance();
            auto right = parseMultiplicative();
            expression = makeExpr(Expr::Kind::Binary, op.lexeme, op.location, {expression, right});
        }
        return expression;
    }

    ExprPtr Parser::parseMultiplicative() {
        auto expression = parseUnary();

        while (check(TokenKind::Star) ||
            check(TokenKind::Slash) ||
            check(TokenKind::Percent)) {

            const Token op = advance();
            auto right = parseUnary();

            expression = makeExpr(
                Expr::Kind::Binary,
                op.lexeme,
                op.location,
                {expression, right}
            );
        }

        return expression;
    }

    ExprPtr Parser::parseUnary() {
        if (match(TokenKind::PlusPlus)) {
            const Token op = tokens_[current_- 1];
            return makeExpr(Expr::Kind::PrefixIncrement, op.lexeme, op.location, {parseUnary()});
        }

        if (match(TokenKind::MinusMinus)) {
            const Token op = tokens_[current_ - 1];
            return makeExpr(Expr::Kind::PrefixDecrement, op.lexeme, op.location, {parseUnary()});
        }

        if (check(TokenKind::Plus) || check(TokenKind::Minus)) {
            const Token op = advance();
            return makeExpr(Expr::Kind::Unary, op.lexeme, op.location, {parseUnary()});
        }
        return parsePower();
    }

    ExprPtr Parser::parsePower() {
        auto expression = parsePostfix();
        if (match(TokenKind::Caret)) {
            const Token op = tokens_[current_ - 1];
            expression = makeExpr(Expr::Kind::Binary, op.lexeme, op.location, {expression, parseUnary()});
        }
        return expression;
    }

    ExprPtr Parser::parsePostfix() {
        auto expression = parsePrimary();
        while (true) {
            if (match(TokenKind::LeftParen)) {
                std::vector<ExprPtr> children{expression};
                if (!check(TokenKind::RightParen)) {
                    do {children.push_back(parseExpression());}
                    while (match(TokenKind::Comma));
                }
                const Token close = peek();
                consume(TokenKind::RightParen, "Expected ')' after function arguments");
                expression = makeExpr(Expr::Kind::Call, "call", close.location, std::move(children));
            } else if (match(TokenKind::Dot)) {
                const Token member = peek();
                if (!consume(TokenKind::Identifier, "Expected a member name after '.'")) break;
                expression = makeExpr(Expr::Kind::Member, member.lexeme, member.location, {expression});
            } else if (match(TokenKind::PlusPlus)) {
                const Token op = tokens_[current_ - 1];
                expression = makeExpr(Expr::Kind::PostfixIncrement, op.lexeme, op.location, {expression});
            } else if (match(TokenKind::MinusMinus)) {
                const Token op = tokens_[current_ - 1];
                expression = makeExpr(Expr::Kind::PostfixDecrement, op.lexeme, op.location, {expression});
            } else break;
        }
        return expression;
    }

    ExprPtr Parser::parsePrimary() {
        const Token token = peek();
        if (match(TokenKind::Number)) return makeExpr(Expr::Kind::Number, token.lexeme, token.location);
        if (match(TokenKind::String)) return makeExpr(Expr::Kind::String, token.lexeme, token.location);
        if (match(TokenKind::True)) return makeExpr(Expr::Kind::Boolean, "true", token.location);
        if (match(TokenKind::False)) return makeExpr(Expr::Kind::Boolean, "false", token.location);
        if (match(TokenKind::Identifier)) return makeExpr(Expr::Kind::Identifier, token.lexeme, token.location);
        if (match(TokenKind::LeftParen)) {
            auto expression = parseExpression();
            consume(TokenKind::RightParen, "Expected ')' after expression");
            return expression;
        }
        error(token, "ParseError", "Expected an expression");
        if (!check(TokenKind::EndOfFile)) advance();
        return makeExpr(Expr::Kind::Number, "0", token.location);
    }
}
