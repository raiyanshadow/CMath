#include "LanguageInternal.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace cmath::language::detail {

namespace {
std::optional<long double> constantReal(const ExprPtr& expression) {
    if (!expression) return std::nullopt;
    try {
        switch (expression->kind) {
            case Expr::Kind::Number: return std::stold(expression->text);
            case Expr::Kind::Unary: {
                auto value = constantReal(expression->children[0]);
                if (!value) return std::nullopt;
                if (expression->text == "-") return -*value;
                return *value;
            }
            case Expr::Kind::Binary: {
                auto left = constantReal(expression->children[0]);
                auto right = constantReal(expression->children[1]);
                if (!left || !right) return std::nullopt;
                if (expression->text == "+") return *left + *right;
                if (expression->text == "-") return *left - *right;
                if (expression->text == "*") return *left * *right;
                if (expression->text == "/") {
                    if (*right == 0) return std::nullopt;
                    return *left / *right;
                }
                if (expression->text == "^") return std::pow(*left, *right);
                return std::nullopt;
            }
            default: return std::nullopt;
        }
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace


std::string typeName(ValueType type) {
    switch (type) {
        case ValueType::Integer: return "Integer";
        case ValueType::Rational: return "Rational";
        case ValueType::Real: return "Real";
        case ValueType::Complex: return "Complex";
        case ValueType::Boolean: return "Boolean";
        case ValueType::String: return "String";
        case ValueType::Unit: return "Unit";
        case ValueType::Error: return "<error>";
    }
    return "<unknown>";
}

std::optional<ValueType> parseTypeName(const std::string& name) {
    if (name == "Integer") return ValueType::Integer;
    if (name == "Rational") return ValueType::Rational;
    if (name == "Real") return ValueType::Real;
    if (name == "Complex") return ValueType::Complex;
    if (name == "Boolean" || name == "Bool") return ValueType::Boolean;
    if (name == "String") return ValueType::String;
    if (name == "Unit") return ValueType::Unit;
    return std::nullopt;
}

bool isNumeric(ValueType type) {
    return type == ValueType::Integer || type == ValueType::Rational ||
           type == ValueType::Real || type == ValueType::Complex;
}

bool canWiden(ValueType from, ValueType to) {
    if (from == to || from == ValueType::Error || to == ValueType::Error) return true;
    if (from == ValueType::Integer)
        return to == ValueType::Rational || to == ValueType::Real || to == ValueType::Complex;
    if (from == ValueType::Rational)
        return to == ValueType::Real || to == ValueType::Complex;
    if (from == ValueType::Real) return to == ValueType::Complex;
    return false;
}

SemanticAnalyzer::SemanticAnalyzer(std::size_t maximumDiagnostics, bool allowShadowing)
    : maximumDiagnostics_(maximumDiagnostics), allowShadowing_(allowShadowing) {}

void SemanticAnalyzer::error(const SourceLocation& location, std::string code,
                             std::string message, std::vector<std::string> notes) {
    if (diagnostics_.size() >= maximumDiagnostics_) return;
    diagnostics_.push_back(Diagnostic{DiagnosticSeverity::Error, std::move(code),
                                      std::move(message), location, std::move(notes)});
}

std::optional<ValueType> SemanticAnalyzer::resolveType(const std::string& name,
                                                        const SourceLocation& loc) {
    auto type = parseTypeName(name);
    if (!type) error(loc, "TypeError", "Unknown type '" + name + "'");
    return type;
}

void SemanticAnalyzer::collectFunctions(const AstProgram& ast) {
    static const std::unordered_set<std::string> reserved{
        "print", "sqrt", "sin", "cos", "tan", "exp", "log", "abs",
        "floor", "approximate", "to_integer_exact", "pi", "e", "Complex",
        "Integer", "Rational", "Real", "Boolean", "Bool", "String", "Unit"
    };
    for (const auto& statement : ast.statements) {
        if (statement.kind != Statement::Kind::Function) continue;
        if (reserved.contains(statement.name)) {
            error(statement.location, "SemanticsError", "Function name '" + statement.name + "' is reserved");
            continue;
        }
        if (signatures_.contains(statement.name)) {
            error(statement.location, "SemanticsError", "Function '" + statement.name + "' is already declared");
            continue;
        }
        FunctionSignature signature;
        bool valid = true;
        std::unordered_set<std::string> parameterNames;
        for (const auto& parameter : statement.parameters) {
            static const std::unordered_set<std::string> reservedParameters{
                "print", "sqrt", "sin", "cos", "tan", "exp", "log", "abs",
                "floor", "approximate", "to_integer_exact", "pi", "e", "Complex",
                "Integer", "Rational", "Real", "Boolean", "Bool", "String", "Unit"
            };
            if (reservedParameters.contains(parameter.name)) {
                error(parameter.location, "SemanticsError", "Parameter name '" + parameter.name + "' is reserved");
                valid = false;
            }
            if (!parameterNames.insert(parameter.name).second) {
                error(parameter.location, "SemanticsError", "Duplicate parameter '" + parameter.name + "'");
                valid = false;
            }
            auto type = resolveType(parameter.typeName, parameter.location);
            if (!type) valid = false;
            else signature.parameters.push_back(*type);
        }
        auto returnType = resolveType(statement.returnType, statement.location);
        if (!returnType) valid = false;
        else signature.result = *returnType;
        if (valid) signatures_.emplace(statement.name, std::move(signature));
    }
}

ValueType SemanticAnalyzer::promotedNumeric(ValueType left, ValueType right) const {
    const auto rank = [](ValueType type) {
        switch (type) {
            case ValueType::Integer: return 0;
            case ValueType::Rational: return 1;
            case ValueType::Real: return 2;
            case ValueType::Complex: return 3;
            default: return -1;
        }
    };
    return rank(left) >= rank(right) ? left : right;
}

IRExprPtr SemanticAnalyzer::makeIR(IRExpression::Kind kind, std::string text,
                                   ValueType type, const SourceLocation& location,
                                   std::vector<IRExprPtr> children) {
    auto result = std::make_shared<IRExpression>();
    result->kind = kind;
    result->text = std::move(text);
    result->type = type;
    result->location = location;
    result->children = std::move(children);
    return result;
}

IRExprPtr SemanticAnalyzer::analyzeExpression(const ExprPtr& expression, const Scope& scope) {
    if (!expression) return makeIR(IRExpression::Kind::Literal, "0", ValueType::Error, {});

    switch (expression->kind) {
        case Expr::Kind::Number: {
            ValueType type = ValueType::Integer;
            if (expression->text.find_first_of("eE") != std::string::npos) type = ValueType::Real;
            else if (expression->text.find('.') != std::string::npos) type = ValueType::Rational;
            return makeIR(IRExpression::Kind::Literal, expression->text, type, expression->location);
        }
        case Expr::Kind::String:
            return makeIR(IRExpression::Kind::Literal, expression->text, ValueType::String,
                          expression->location);
        case Expr::Kind::Boolean:
            return makeIR(IRExpression::Kind::Literal, expression->text, ValueType::Boolean,
                          expression->location);
        case Expr::Kind::Identifier: {
            const auto found = scope.find(expression->text);
            if (found != scope.end())
                return makeIR(IRExpression::Kind::Variable, expression->text,
                              found->second.type, expression->location);
            if (expression->text == "pi" || expression->text == "e")
                return makeIR(IRExpression::Kind::Variable, expression->text,
                              ValueType::Real, expression->location);
            error(expression->location, "RuntimeError", "Unknown name '" + expression->text + "'");
            return makeIR(IRExpression::Kind::Variable, expression->text,
                          ValueType::Error, expression->location);
        }
        case Expr::Kind::Member: {
            const auto& base = expression->children.front();
            if (base->kind == Expr::Kind::Identifier && base->text == "Complex" &&
                (expression->text == "i" || expression->text == "Imaginary")) {
                return makeIR(IRExpression::Kind::ComplexImaginary, expression->text,
                              ValueType::Complex, expression->location);
            }
            error(expression->location, "RuntimeError", "Unknown mathematical member '" + expression->text + "'");
            return makeIR(IRExpression::Kind::Variable, expression->text,
                          ValueType::Error, expression->location);
        }
        case Expr::Kind::Unary: {
            auto operand = analyzeExpression(expression->children[0], scope);
            if (!isNumeric(operand->type)) {
                error(expression->location, "TypeError", "Unary '" + expression->text +
                      "' requires a numeric operand, not " + typeName(operand->type) + "");
            }
            return makeIR(IRExpression::Kind::Unary, expression->text, operand->type,
                          expression->location, {operand});
        }
        case Expr::Kind::Binary: {
            auto left = analyzeExpression(expression->children[0], scope);
            auto right = analyzeExpression(expression->children[1], scope);
            const std::string& op = expression->text;
            ValueType result = ValueType::Error;
            if (op == "/") {
                const auto constantDenominator = constantReal(expression->children[1]);
                if (constantDenominator && *constantDenominator == 0)
                    error(expression->location, "DomainError", "Division by zero is statically known for this expression");
            }
            if (op == "+" && left->type == ValueType::String && right->type == ValueType::String) {
                result = ValueType::String;
            } else if (op == "+" || op == "-" || op == "*" || op == "/" || op == "^" || op == "%") {
                if (!isNumeric(left->type) || !isNumeric(right->type)) {
                    error(expression->location, "TypeError", "Operator '" + op + "' requires numeric operands; received " +
                          typeName(left->type) + " and " + typeName(right->type));
                } else if (op == "%") {
                    if (left->type != ValueType::Integer || right->type != ValueType::Integer)
                        error(expression->location, "TypeError", "Operator '%' is defined only for Integer operands");
                    else result = ValueType::Integer;
                } else if (op == "^") {
                    if (right->type != ValueType::Integer) {
                        error(expression->location, "TypeError", "The exponent must have type Integer");
                    } else if (left->type == ValueType::Integer) {
                        const auto knownExponent = constantReal(expression->children[1]);
                        result = knownExponent && *knownExponent >= 0 &&
                                 std::floor(*knownExponent) == *knownExponent
                            ? ValueType::Integer : ValueType::Rational;
                    } else {
                        result = left->type;
                    }
                } else {
                    result = promotedNumeric(left->type, right->type);
                    if (op == "/" && result != ValueType::Real && result != ValueType::Complex)
                        result = ValueType::Rational;
                }
            } else {
                const bool equality = op == "==" || op == "!=";
                if (isNumeric(left->type) && isNumeric(right->type)) {
                    if (!equality && (left->type == ValueType::Complex || right->type == ValueType::Complex))
                        error(expression->location, "DomainError", "Complex numbers support equality comparisons only");
                    result = ValueType::Boolean;
                } else if (left->type == right->type &&
                           (left->type == ValueType::String || left->type == ValueType::Boolean) && equality) {
                    result = ValueType::Boolean;
                } else {
                    error(expression->location, "DomainError", "Incompatible operands for comparison: " +
                          typeName(left->type) + " and " + typeName(right->type));
                }
            }
            return makeIR(IRExpression::Kind::Binary, op, result, expression->location,
                          {left, right});
        }
        case Expr::Kind::Call: {
            IRExprPtr result;
            analyzeCall(expression, scope, result);
            return result;
        }
    }
    return makeIR(IRExpression::Kind::Literal, "0", ValueType::Error, expression->location);
}

std::optional<ValueType> SemanticAnalyzer::analyzeCall(const ExprPtr& expression,
                                                        const Scope& scope,
                                                        IRExprPtr& result) {
    if (expression->children.empty() ||
        expression->children[0]->kind != Expr::Kind::Identifier) {
        error(expression->location, "RuntimeError", "Only named functions can be called");
        result = makeIR(IRExpression::Kind::Call, "", ValueType::Error, expression->location);
        return ValueType::Error;
    }
    const std::string name = expression->children[0]->text;
    std::vector<IRExprPtr> arguments;
    for (std::size_t i = 1; i < expression->children.size(); ++i)
        arguments.push_back(analyzeExpression(expression->children[i], scope));

    auto arity = [&](std::size_t expected) {
        if (arguments.size() != expected) {
            error(expression->location, "RuntimeError", "Function '" + name + "' expects " +
                  std::to_string(expected) + " argument(s), but received " +
                  std::to_string(arguments.size()));
            return false;
        }
        return true;
    };
    auto numericArgument = [&]() {
        return !arguments.empty() && isNumeric(arguments[0]->type);
    };

    ValueType returnType = ValueType::Error;
    if (const auto function = signatures_.find(name); function != signatures_.end()) {
        if (arguments.size() != function->second.parameters.size()) {
            error(expression->location, "RuntimeError", "Function '" + name + "' received the wrong number of arguments");
        } else {
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                if (!canWiden(arguments[i]->type, function->second.parameters[i])) {
                    error(arguments[i]->location, "RuntimeError", "Argument " + std::to_string(i + 1) +
                          " of '" + name + "' has type " + typeName(arguments[i]->type) +
                          "; expected " + typeName(function->second.parameters[i]));
                }
            }
        }
        returnType = function->second.result;
    } else if (name == "print") {
        returnType = ValueType::Unit;
    } else if (name == "sqrt" || name == "sin" || name == "cos" || name == "tan" ||
               name == "exp" || name == "log" || name == "abs" || name == "floor" ||
               name == "approximate" || name == "to_integer_exact") {
        if (arity(1) && !numericArgument())
            error(expression->location, "RuntimeError", "Function '" + name + "' requires a numeric argument");
        if (!arguments.empty() && isNumeric(arguments[0]->type)) {
            const auto input = arguments[0]->type;
            if (name == "floor" && input == ValueType::Complex)
                error(expression->location, "DomainError", "floor() is not defined for Complex values");
            if ((name == "sqrt" || name == "log") && input != ValueType::Complex &&
                defaultDomain_ != "Complex") {
                const auto constant = constantReal(expression->children[1]);
                if (constant && name == "sqrt" && *constant < 0)
                    error(expression->location, "DomainError", "The real square root of a negative value is undefined; use 'domain Complex' or a Complex argument");
                if (constant && name == "log" && *constant <= 0)
                    error(expression->location, "DomainError", "The real logarithm requires a strictly positive argument");
            }
            if (name == "sqrt")
                returnType = input == ValueType::Complex || defaultDomain_ == "Complex"
                    ? ValueType::Complex : ValueType::Real;
            else if (name == "sin" || name == "cos" || name == "tan" || name == "exp" || name == "log")
                returnType = input == ValueType::Complex || defaultDomain_ == "Complex"
                    ? ValueType::Complex : ValueType::Real;
            else if (name == "abs") returnType = input == ValueType::Complex ? ValueType::Real : input;
            else if (name == "floor" || name == "to_integer_exact") returnType = ValueType::Integer;
            else if (name == "approximate") returnType = input == ValueType::Complex ? ValueType::Complex : ValueType::Real;
        }
    } else {
        error(expression->location, "RuntimeError", "Unknown function '" + name + "'");
    }

    result = makeIR(IRExpression::Kind::Call, name, returnType, expression->location,
                    std::move(arguments));
    return returnType;
}

std::vector<IRStatement> SemanticAnalyzer::analyzeStatements(
    const std::vector<Statement>& statements, Scope& scope, bool inFunction,
    ValueType returnType, bool& sawReturn) {
    std::vector<IRStatement> result;
    for (const auto& statement : statements) {
        IRStatement ir;
        ir.location = statement.location;
        switch (statement.kind) {
            case Statement::Kind::Variable: {
                static const std::unordered_set<std::string> reservedNames{
                    "print", "sqrt", "sin", "cos", "tan", "exp", "log", "abs",
                    "floor", "approximate", "to_integer_exact", "pi", "e", "Complex",
                    "Integer", "Rational", "Real", "Boolean", "Bool", "String", "Unit"
                };
                if (reservedNames.contains(statement.name) || signatures_.contains(statement.name))
                    error(statement.location, "SemanticsError", "Name '" + statement.name + "' is reserved by a built-in or function declaration");
                if (scope.contains(statement.name) && !allowShadowing_) {
                    error(statement.location, "RuntimeError", "Name '" + statement.name + "' is already declared in this scope");
                }
                auto initializer = analyzeExpression(statement.expression, scope);
                ValueType declared = initializer->type;
                if (!statement.typeName.empty()) {
                    auto explicitType = resolveType(statement.typeName, statement.location);
                    if (explicitType) {
                        declared = *explicitType;
                        if (!canWiden(initializer->type, declared)) {
                            error(statement.expression->location, "RuntimeError", "Cannot assign " +
                                  typeName(initializer->type) + " to " + typeName(declared) + ".",
                                  {"Use a wider declared type or an explicit checked conversion"});
                        }
                    }
                }
                scope[statement.name] = SymbolInfo{declared, statement.mutableBinding};
                ir.kind = IRStatement::Kind::Declare;
                ir.name = statement.name;
                ir.type = declared;
                ir.mutableBinding = statement.mutableBinding;
                ir.expression = initializer;
                result.push_back(std::move(ir));
                break;
            }
            case Statement::Kind::Assignment: {
                auto found = scope.find(statement.name);
                if (found == scope.end()) {
                    error(statement.location, "RuntimeError", "Cannot assign to undeclared variable '" + statement.name + "'");
                    break;
                }
                if (!found->second.mutableBinding)
                    error(statement.location, "RuntimeError", "Cannot reassign immutable binding '" + statement.name + "'; declare it with 'var'");
                auto value = analyzeExpression(statement.expression, scope);
                if (!canWiden(value->type, found->second.type))
                    error(statement.expression->location, "RuntimeError", "Cannot assign " + typeName(value->type) +
                          " to variable '" + statement.name + "' of type " + typeName(found->second.type));
                ir.kind = IRStatement::Kind::Assign;
                ir.name = statement.name;
                ir.type = found->second.type;
                ir.expression = value;
                result.push_back(std::move(ir));
                break;
            }
            case Statement::Kind::Expression: {
                ir.kind = IRStatement::Kind::Expression;
                ir.expression = analyzeExpression(statement.expression, scope);
                result.push_back(std::move(ir));
                break;
            }
            case Statement::Kind::Return: {
                if (!inFunction) {
                    error(statement.location, "RuntimeError", "'return' can only appear inside a function");
                    break;
                }
                sawReturn = true;
                if (returnType == ValueType::Unit) {
                    if (statement.expression)
                        error(statement.location, "RuntimeError", "A Unit function cannot return a value");
                    ir.type = ValueType::Unit;
                } else {
                    if (!statement.expression) {
                        error(statement.location, "RuntimeError", "A function returning " + typeName(returnType) + " must return a value");
                        ir.type = returnType;
                    } else {
                        ir.expression = analyzeExpression(statement.expression, scope);
                        if (!canWiden(ir.expression->type, returnType))
                            error(statement.expression->location, "RuntimeError", "Function returns " +
                                  typeName(ir.expression->type) + "; declared return type is " + typeName(returnType));
                        ir.type = returnType;
                    }
                }
                ir.kind = IRStatement::Kind::Return;
                result.push_back(std::move(ir));
                break;
            }
            case Statement::Kind::Function:
                error(statement.location, "RuntimeError", "Nested function definitions is not supported");
                break;
        }
    }
    return result;
}

void SemanticAnalyzer::analyzeFunction(const Statement& statement) {
    const auto signatureIt = signatures_.find(statement.name);
    if (signatureIt == signatures_.end()) return;
    Scope scope = globals_;
    IRFunction function;
    function.name = statement.name;
    function.returnType = signatureIt->second.result;
    function.location = statement.location;

    for (std::size_t i = 0; i < statement.parameters.size(); ++i) {
        const auto& parameter = statement.parameters[i];
        const auto type = signatureIt->second.parameters[i];
        if (scope.contains(parameter.name) && !allowShadowing_)
            error(parameter.location, "RuntimeError", "Parameter '" + parameter.name + "' shadows an existing global name");
        scope[parameter.name] = SymbolInfo{type, false};
        function.parameters.push_back(IRParameter{parameter.name, type});
    }

    bool sawReturn = false;
    function.body = analyzeStatements(statement.body, scope, true, function.returnType, sawReturn);
    if (!sawReturn)
        error(statement.location, "RuntimeError", "Function '" + statement.name + "' must contain a return statement");
    if (statement.body.empty() || statement.body.back().kind != Statement::Kind::Return)
        error(statement.location, "RuntimeError", "A function's return statement must be its final statement");
    functions_[statement.name] = std::move(function);
}

SemanticResult SemanticAnalyzer::analyze(const AstProgram& ast) {
    diagnostics_.clear();
    signatures_.clear();
    functions_.clear();
    globals_.clear();
    defaultDomain_ = ast.defaultDomain;

    static const std::unordered_set<std::string> domains{"Integer", "Rational", "Real", "Complex"};
    if (!domains.contains(defaultDomain_))
        error(ast.domainLocation, "DomainError", "Unknown default domain '" + defaultDomain_ +
              "'. Supported domains in Language 0.1: Integer, Rational, Real, Complex");

    collectFunctions(ast);
    auto program = std::make_shared<ProgramIR>();
    program->defaultDomain = defaultDomain_;

    // Process top-level declarations in source order. Functions can call later
    // function declarations, but can only reference globals declared earlier.
    for (const auto& statement : ast.statements) {
        if (statement.kind == Statement::Kind::Function) {
            analyzeFunction(statement);
            continue;
        }
        Scope globalScope = globals_;
        bool sawReturn = false;
        auto lowered = analyzeStatements({statement}, globalScope, false,
                                         ValueType::Error, sawReturn);
        if (statement.kind == Statement::Kind::Variable && globalScope.contains(statement.name))
            globals_[statement.name] = globalScope[statement.name];
        program->statements.insert(program->statements.end(), lowered.begin(), lowered.end());
    }
    program->functions = functions_;

    if (!diagnostics_.empty()) program.reset();
    return SemanticResult{std::move(program), std::move(diagnostics_)};
}

} // namespace cmath::language::detail
