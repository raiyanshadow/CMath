#pragma once

#include <complex>
#include <cstdint> 
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cmath/language/Diagnostic.h"
#include "cmath/language/ExecutionOptions.h"

namespace cmath::language::detail {
    enum class TokenKind {
        Identifier, Number, String, 
        Let, Var, Def, Return, Domain, True, False,
        Plus, Minus, Star, Slash, Percent, Caret,
        Equal, EqualEqual, NotEqual, Less, Greater, LessEqual, GreaterEqual,
        LeftParen, RightParen, Comma, Colon, Dot, Arrow,
        Newline, Indent, Dedent, EndOfFile, Invalid
    };

    struct Token {
        TokenKind kind = TokenKind::Invalid;
        std::string lexeme;
        SourceLocation location{};
    };

    struct LexResult {
        std::vector<Token> tokens;
        std::vector<Diagnostic> diagnostics;
    };

    class Lexer {
        public:
            explicit Lexer(std::string_view source, std::size_t maximumDiagnostics = 1000);
            LexResult tokenize();

        private:
            bool atEnd() const;
            char peek(std::size_t lookahead = 0) const;
            char advance();
            bool match(char expected);
            void add(TokenKind kind, std::size_t start, SourceLocation location);
            void scanToken();
            void scanIdentifier();
            void scanNumber();
            void scanString();
            void processIndentation();
            void error(std::string code, std::string message, SourceLocation location);

            std::string_view source_;
            std::vector<Token> tokens_;
            std::vector<Diagnostic> diagnostics_;
            std::vector<std::size_t> indentation_{0};
            std::vector<std::size_t> alternateIndentation_{0};
            std::size_t current_ = 0;
            std::size_t line_ = 1;
            std::size_t column_ = 1;
            bool lineHasCode_ = false;
            bool atLineStart_ = true;
            std::size_t maximumDiagnostics_ = 1000;
            int bracketDepth_ = 0;
    };

    struct Expr {
        enum class Kind {Number, String, Boolean, Identifier, Unary, Binary, Call, Member};
        Kind kind = Kind::Number;
        std::string text;
        SourceLocation location{};
        std::vector<std::shared_ptr<Expr>> children; 
    };
    using ExprPtr = std::shared_ptr<Expr>;

    struct ParameterAst {
        std::string name;
        std::string typeName;
        SourceLocation location{};
    };

    struct Statement {
        enum class Kind {Variable, Function, Return, Expression, Assignment};
        Kind kind = Kind::Expression;
        SourceLocation location{};
        std::string name;
        std::string typeName;
        bool mutableBinding = false;
        ExprPtr expression;
        std::vector<ParameterAst> parameters;
        std::string returnType;
        std::vector<Statement> body;
    };

    struct AstProgram {
        std::string defaultDomain;
        SourceLocation domainLocation{};
        std::vector<Statement> statements;
    };

    struct ParseResult {
        std::optional<AstProgram> program;
        std::vector<Diagnostic> diagnostics;
    };

    class Parser {
        public:
            explicit Parser(const std::vector<Token>& tokens, std::size_t maximumDiagnostics);
            ParseResult parse();

        private:
            const Token& peek(std::size_t lookahead = 0) const;
            bool check(TokenKind kind) const;
            bool match(TokenKind kind);
            const Token& advance();
            bool consume(TokenKind kind, const std::string& message);
            void error(const Token& token, std::string code, std::string message);
            void synchronize();
            Statement parseStatement();
            Statement parseVariable();
            Statement parseFunction();
            Statement parseReturn();
            Statement parseAssignmentOrExpression();
            std::vector<Statement> parseIndentedBlock();
            ExprPtr parseExpression();
            ExprPtr parseComparison();
            ExprPtr parseAdditive();
            ExprPtr parseMultiplicative();
            ExprPtr parseUnary();
            ExprPtr parsePower();
            ExprPtr parsePostfix();
            ExprPtr parsePrimary();
            ExprPtr makeExpr(Expr::Kind kind, std::string text, SourceLocation location,
                                std::vector<ExprPtr> children = {});

            const std::vector<Token>& tokens_;
            std::vector<Diagnostic> diagnostics_;
            std::size_t current_ = 0;
            std::size_t maximumDiagnostics_;
    };

    enum class ValueType {Integer, Rational, Real, Complex, Boolean, String, Unit, Error};
    std::string typeName(ValueType type);
    std::optional<ValueType> parseTypeName(const std::string& name);
    bool isNumeric(ValueType type);
    bool canWiden(ValueType from, ValueType to);

    struct IRExpression {
        enum class Kind {Literal, Variable, Unary, Binary, Call, ComplexImaginary};
        Kind kind = Kind::Literal;
        std::string text;
        ValueType type = ValueType::Error;
        SourceLocation location{};
        std::vector<std::shared_ptr<IRExpression>> children;
    };
    using IRExprPtr = std::shared_ptr<IRExpression>;

    struct IRStatement {
        enum class Kind {Declare, Assign, Expression, Return};
        Kind kind = Kind::Expression;
        SourceLocation location{};
        std::string name;
        ValueType type = ValueType::Error;
        bool mutableBinding = false;
        IRExprPtr expression;
    };

    struct IRParameter {
        std::string name;
        ValueType type = ValueType::Error;
    };

    struct IRFunction {
        std::string name;
        std::vector<IRParameter> parameters;
        ValueType returnType = ValueType::Error;
        std::vector<IRStatement> body;
        SourceLocation location{};
    };

    struct ProgramIR {
        std::string defaultDomain;
        std::vector<IRStatement> statements;
        std::unordered_map<std::string, IRFunction> functions;
    };

    struct SemanticResult {
        std::shared_ptr<ProgramIR> program;
        std::vector<Diagnostic> diagnostics;
    };

    class SemanticAnalyzer {
        public:
            SemanticAnalyzer(std::size_t maximumDiagnostics, bool allowShadowing);
            SemanticResult analyze(const AstProgram& ast);

        private:
            struct SymbolInfo {ValueType type; bool mutableBinding;};
            using Scope = std::unordered_map<std::string, SymbolInfo>;
            struct FunctionSignature {std::vector<ValueType> parameters; ValueType result;};

            void error(const SourceLocation& location, std::string code, std::string message,
            std::vector<std::string> notes = {});
            std::optional<ValueType> resolveType(const std::string& name, const SourceLocation& loc);
            void collectFunctions(const AstProgram& ast);
            void analyzeFunction(const Statement& statement);
            std::vector<IRStatement> analyzeStatements(const std::vector<Statement>& statements,
                                                        Scope& scope, bool inFunction,
                                                        ValueType returnType, bool& sawReturn);
            IRExprPtr analyzeExpression(const ExprPtr& expression, const Scope& scope);
            IRExprPtr makeIR(IRExpression::Kind kind, std::string text, ValueType type,
                            const SourceLocation& location, std::vector<IRExprPtr> children = {});
            std::optional<ValueType> analyzeCall(const ExprPtr &expression, const Scope & scope, 
                                                 IRExprPtr& result);
            ValueType promotedNumeric(ValueType left, ValueType right) const;

            std::size_t maximumDiagnostics_;
            bool allowShadowing_;
            std::vector<Diagnostic> diagnostics_;
            std::unordered_map<std::string, FunctionSignature> signatures_;
            std::unordered_map<std::string, IRFunction> functions_;
            std::unordered_map<std::string, SymbolInfo> globals_;
            std::string defaultDomain_;
    };

    struct RationalValue {
        std::int64_t numerator = 0;
        std::int64_t denominator = 1;
    };
    using RuntimeData = std::variant<std::int64_t, RationalValue, double,
                                    std::complex<double>, bool, std::string,
                                    std::monostate>;
    struct RuntimeValue {
        ValueType type = ValueType::Unit;
        RuntimeData data = std::monostate{};
    };

    struct RuntimeResult {
        bool succeeded = false;
        std::string output;
        std::optional<std::string> lastValue;
        std::vector<Diagnostic> diagnostics;
    };

    class Interpreter {
        public:
            explicit Interpreter(const ExecutionOptions& options);
            RuntimeResult execute(const ProgramIR& program);
        private:
            struct Environment {
                std::unordered_map<std::string, RuntimeValue> values;
                std::unordered_map<std::string, bool> mutableBindings;
                Environment* parent = nullptr;
            };

            struct ReturnSignal {RuntimeValue value;};

            RuntimeValue evaluate(const IRExprPtr& expression, Environment& environment);
            RuntimeValue executeStatement(const IRStatement& statement, Environment& environment,
                                            bool& sawReturn);   
            RuntimeValue callFunction(const std::string& name,
                                      const std::vector<RuntimeValue>& arguments,
                                      const SourceLocation& location);
            RuntimeValue callBuiltin(const std::string& name,
                                     const std::vector<RuntimeValue>& arguments,
                                     const SourceLocation& location);
            RuntimeValue applyUnary(const std::string& op, const RuntimeValue& value,
                                    const SourceLocation& location);
            RuntimeValue applyBinary(const std::string& op, const RuntimeValue& left,
                                     const RuntimeValue& right, ValueType resultType,
                                     const SourceLocation& location);
            RuntimeValue promote(const RuntimeValue& value, ValueType target,
                                 const SourceLocation& location);
            void runtimeError(const SourceLocation& location, std::string code, std::string message);
            std::string format(const RuntimeValue& value) const;
            bool consumeInstruction(const SourceLocation& location);

            const ProgramIR* program_ = nullptr;
            ExecutionOptions options_;
            std::size_t instructionCount_ = 0;
            std::size_t callDepth_ = 0;
            Environment globals_;
            std::string output_;
            std::optional<std::string> lastValue_;
            std::vector<Diagnostic> diagnostics_;
    };
}