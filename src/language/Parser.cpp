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

    void Parser::synchronize() {
        while (!check(TokenKind::EndOfFile)) {
            if (check(TokenKind::Newline)) {advance(); return;}
            if (check(TokenKind::Dedent)) return;
            if (check(TokenKind::Let) || check(TokenKind::Var) || check(TokenKind::Def) || check(TokenKind::Return)) return;
            advance();
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
            try {
                program.statements.push_back(parseStatement());
            } catch (...) {
                synchronize();
            } 
            if (current_ == before) advance();
        }

        if (!diagnostics_.empty()) return ParseResult{std::nullopt, std::move(diagnostics_)};
        return ParseResult{std::move(program), std::move(diagnostics_)};
    }

    Statement Parser::parseStatement() {
        if (check(TokenKind::Let) || check(TokenKind::Var)) return parseVariable();
        if (check(TokenKind::Def)) return parseFunction();
        if (check(TokenKind::Return)) return parseReturn();
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
            if (!consume(TokenKind::Identifier, "Expected a type name after ':'"))
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
        const Token name = peek();
        consume(TokenKind::Identifier, "Expected a function name after 'def'");
        Statement statement;
        statement.kind = Statement::Kind::Function;
        statement.location = keyword.location;
        statement.name = name.lexeme;
        consume(TokenKind::LeftParen, "Expected '(' after function name");
        if (!check(TokenKind::RightParen)) {
            do {
                const Token parameterName = peek();
                if (!consume(TokenKind::Identifier, "Expected parameter name")) break;
                consume(TokenKind::Colon, "Function parameters must have type annotation");
                const Token type = peek();
                if (!consume(TokenKind::Identifier, "Expected parameter type")) break;
                statement.parameters.push_back(ParameterAst{parameterName.lexeme, type.lexeme, parameterName.location});
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RightParen, "Expected ')' after function parameters.");
        if (match(TokenKind::Arrow)) {
            const Token resultType = peek();
            if (consume(TokenKind::Identifier, "Expected return type after '->'")) statement.returnType = resultType.lexeme;
        } else {
            error(peek(), "ParseError", "Function return types are required");
        }
        consume(TokenKind::Colon, "Expected ':' after function signature");
        statement.body = parseIndentedBlock();
        return statement;
    }

    std::vector<Statement> Parser::parseIndentedBlock() {
        std::vector<Statement> body;
        consume(TokenKind::Newline, "Expected a newline before the function body");
        if (!consume(TokenKind::Indent, "Expected an indented function body")) return body;
        while (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
            if (match(TokenKind::Newline)) continue;
            const auto before = current_;
            body.push_back(parseStatement());
            if (current_ == before) advance();
        }
        consume(TokenKind::Dedent, "Expected the end of the funtion body");
        return body;
    }

    Statement Parser::parseReturn() {
        const Token keyword = advance();
        Statement statement;
        statement.kind = Statement::Kind::Return;
        statement.location = keyword.location;
        if (!check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) 
            statement.expression = parseExpression();
        match(TokenKind::Newline);
        return statement;
    }

    Statement Parser::parseAssignmentOrExpression() {
        if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Equal) {
            const Token name = advance();
            advance();
            Statement statement;
            statement.kind = Statement::Kind::Assignment;
            statement.location = name.location;
            statement.name = name.lexeme;
            statement.expression = parseExpression();
            if (!check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile))
                statement.expression = parseExpression();
            match(TokenKind::Newline);
            return statement;
        }
        Statement statement;
        statement.kind = Statement::Kind::Expression;
        statement.location = peek().location;
        statement.expression = parseExpression();
        if (!check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile))
            error(peek(), "ParseError", "Unexpected token after expression");
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

    ExprPtr Parser::parseExpression() {return parseComparison();}
    ExprPtr Parser::parseComparison() {
        auto expression = parseAdditive();
        while (check(TokenKind::EqualEqual) || check(TokenKind::NotEqual) ||
               check(TokenKind::Less) || check(TokenKind::Greater) ||
               check(TokenKind::LessEqual) || check(TokenKind::GreaterEqual)) {
            const Token op = advance();
            auto right = parseAdditive();
            expression = makeExpr(Expr::Kind::Binary, op.lexeme, op.location, {expression, right});
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
            } else break;
        }
        return expression;
    }

    ExprPtr Parser::parsePrimary() {
        const Token token = peek();
        if (match(TokenKind::Number)) return makeExpr(Expr::Kind::Number, token.lexeme, token.location);
        if (match(TokenKind::String)) return makeExpr(Expr::Kind::Boolean, token.lexeme, token.location);
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
