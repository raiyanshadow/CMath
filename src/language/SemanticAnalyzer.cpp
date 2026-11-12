#include "LanguageInternal.h"

#include <cmath>
#include <unordered_set>
#include <utility>
#include <iterator>

namespace cmath::language::detail {

namespace {
std::optional<long double> constantReal(const ExprPtr& expression) {
    if (!expression) return std::nullopt;
    try {
        switch (expression->kind) {
            case Expr::Kind::Number: return std::stold(expression->text);
            case Expr::Kind::Unary: {
                if (expression->children.empty()) return std::nullopt;
                auto value = constantReal(expression->children[0]);
                if (!value) return std::nullopt;
                if (expression->text == "-") return -*value;
                if (expression->text == "+") return *value;
                return std::nullopt;
            }
            case Expr::Kind::Binary: {
                if (expression->children.size() < 2) return std::nullopt;

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

bool isReservedName(const std::string& name) {
    static const std::unordered_set<std::string> reserved{
        "print", "sqrt", "sin", "cos", "tan", "exp", "log", "abs",
        "floor", "approximate", "to_integer_exact", "pi", "e",
        "Complex", "Integer", "Rational", "Real", "Boolean", "Bool",
        "String", "Unit"
    };

    return reserved.contains(name);
}


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

const SemanticAnalyzer::SymbolInfo* SemanticAnalyzer::findSymbol(const Scope& scope, const std::string& name) const {
    const Scope* current = &scope;
    while (current != nullptr) {
        const auto found = current->symbols.find(name);

        if (found != current->symbols.end()) 
            return &found->second;
        current = current->parent;
    }

    return nullptr;
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

IRExprPtr SemanticAnalyzer::makeIR(IRExpression::Kind kind, std::string text, ValueType type, const SourceLocation& location, std::vector<IRExprPtr> children) {
    auto result = std::make_shared<IRExpression>();

    result->kind = kind;
    result->text = std::move(text);
    result->type = type;
    result->location = location;
    result->children = std::move(children);

    return result;
}

void SemanticAnalyzer::collectFunctions(const AstProgram& ast) {
    for (const auto& statement : ast.body.statements) {
        if (statement.kind != Statement::Kind::Function) continue;
        if (isReservedName(statement.name)) {
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
            if (isReservedName(parameter.name)) {
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
            const auto* symbol = findSymbol(scope, expression->text);
            if (symbol)
                return makeIR(IRExpression::Kind::Variable, expression->text,
                              symbol->type, expression->location);
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
        case Expr::Kind::Range: {
            error(expression->location, "TypeError", "Range expressions may only be used as a for-loop range");
            return makeIR(IRExpression::Kind::Literal, "0", ValueType::Error, expression->location);
        }
        case Expr::Kind::Unary: {
            auto operand = analyzeExpression(expression->children[0], scope);
            if (expression->text == "not") {
                if (operand->type != ValueType::Boolean &&
                operand->type != ValueType::Error) {
                    error(expression->location, "TypeError", "'not' requires a Boolean operand");
                }
                return makeIR(IRExpression::Kind::Unary, expression->text, ValueType::Boolean, expression->location, {operand});
            }
            if (!isNumeric(operand->type) && operand->type != ValueType::Error) {
                error(expression->location, "TypeError", "Unary '" + expression->text +
                      "' requires a numeric operand, not " + typeName(operand->type));
            }
            return makeIR(IRExpression::Kind::Unary, expression->text, operand->type,
                          expression->location, {operand});
        }
        case Expr::Kind::PrefixIncrement:
        case Expr::Kind::PostfixIncrement:
        case Expr::Kind::PrefixDecrement:
        case Expr::Kind::PostfixDecrement: {
            const bool postfix = expression->kind == Expr::Kind::PostfixIncrement || expression->kind == Expr::Kind::PostfixDecrement;

            if (expression->children.empty() || expression->children.front()->kind != Expr::Kind::Identifier) {
                error(expression->location, "TypeError", "Increment and decrement requires mutable variable");
                auto invalid = makeIR(IRExpression::Kind::Increment, expression->text, ValueType::Error, expression->location);
                invalid->postfix = postfix;
                return invalid;
            }

            const auto& target = expression->children.front();
            const auto* symbol = findSymbol(scope, target->text);

            if (!symbol) {
                error(expression->location, "NameError", "Cannot increment or decrement unknown variable '" + target->text + "'");
                return makeIR(IRExpression::Kind::Increment, expression->text, ValueType::Error, expression->location);
            }

            if (!symbol->mutableBinding) {
                error(expression->location, "MutabilityError", "Cannot increment or decrement immutable variable '" + target->text + "'");
            }

            if (!isNumeric(symbol->type)) {
                error(expression->location, "TypeError", "Increment and decrement require a numeric variable");
            }

            auto variable = makeIR(IRExpression::Kind::Variable, target->text, symbol->type, target->location);
            auto result = makeIR(IRExpression::Kind::Increment, expression->text, symbol->type, expression->location, {variable});

            result->postfix = postfix;
            return result;
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
            } else if (op == "and" || op == "or") {
                if ((left->type != ValueType::Boolean && left->type != ValueType::Error) || (right->type != ValueType::Boolean && right->type != ValueType::Error)) {
                    error(expression->location, "TypeError", "Logical operator '" + op + "' requires Boolean operands");
                }
                result = ValueType::Boolean;
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
        error(expression->location, "NameError", "Only named functions can be called");
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

IRBlock SemanticAnalyzer::analyzeBlock(const AstBlock& block, Scope& scope, bool inFunction, ValueType returnType, std::size_t loopDepth) {
    IRBlock result;
    result.location = block.location;

    bool reachable = true;

    auto append = [&](IRStatementData data, const SourceLocation& location) {
        result.statements.push_back(IRStatement{location, std::move(data)});
    };

    auto mergeFlow = [&](const FlowSummary& flow) {
        if (!reachable)
            return;

        result.flow.mayReturn = result.flow.mayReturn || flow.mayReturn;

        result.flow.mayBreak = result.flow.mayBreak || flow.mayBreak;

        result.flow.mayContinue = result.flow.mayContinue || flow.mayContinue;

        if (!flow.canFallThrough)
            reachable = false;
    };

    for (const auto& statement : block.statements) {
        FlowSummary statementFlow;

        switch (statement.kind) {
            case Statement::Kind::Variable: {
                if (isReservedName(statement.name) || signatures_.contains(statement.name)) {
                    error(statement.location, "SemanticError", "Name '" + statement.name + "' is reserved by a built-in or function declaration");
                }

                if (scope.symbols.contains(statement.name)) {
                    error(statement.location, "NameError", "Name '" + statement.name + "' is already declared in this scope");
                } else if (!allowShadowing_ && scope.parent && findSymbol(*scope.parent, statement.name)) {
                    error(statement.location, "NameError", "Declaration of '" + statement.name + "' shadows an existing name");
                }

                auto initializer = analyzeExpression(statement.expression, scope);
                ValueType declared = initializer->type;

                if (!statement.typeName.empty()) {
                    auto explicitType = resolveType(statement.typeName, statement.location);
                    if (explicitType) {
                        declared = *explicitType;
                        if (!canWiden(initializer->type, declared)) {
                            error(statement.expression ? statement.expression->location : statement.location, "TypeError", 
                            "Cannot assign " + typeName(initializer->type) + " to " +  typeName(declared));
                        }
                    }
                }

                if (!scope.symbols.contains(statement.name)) {
                    scope.symbols.emplace(statement.name, SymbolInfo{declared, statement.mutableBinding});
                }

                IRDeclaration declaration;
                declaration.name = statement.name;
                declaration.type = declared;
                declaration.mutableBinding = statement.mutableBinding;
                declaration.initializer = std::move(initializer);

                append(std::move(declaration), statement.location);
                break;
            }

            case Statement::Kind::Assignment: {
                const auto* symbol = findSymbol(scope, statement.name);

                if (!symbol) {
                    error(statement.location, "NameError", "Cannot assign to undeclared variable '" + statement.name + "'");
                    break;
                }

                if (!symbol->mutableBinding) {
                    error(statement.location, "MutabilityError", "Cannot modify immutable variable '" + statement.name + "'");
                }

                ExprPtr valueExpression = statement.expression;

                if (statement.assignmentOperator != "=") {
                    const std::string op = statement.assignmentOperator.substr(0, 1);

                    auto left = std::make_shared<Expr>();
                    left->kind = Expr::Kind::Identifier;
                    left->text = statement.name;
                    left->location = statement.location;

                    auto binary = std::make_shared<Expr>();
                    binary->kind = Expr::Kind::Binary;
                    binary->text = op;
                    binary->location = statement.location;
                    binary->children = {
                        left,
                        statement.expression
                    };
                    valueExpression = std::move(binary);
                }

                auto value = analyzeExpression(valueExpression, scope);

                if (!canWiden(value->type, symbol->type)) {
                    error(statement.location, "TypeError", "Cannot assign " + typeName(value->type) + " to variable '" + statement.name + "' of type " + typeName(symbol->type));
                }

                IRAssignment assignment;
                assignment.name = statement.name;
                assignment.type = symbol->type;
                assignment.expression = std::move(value);

                append(std::move(assignment), statement.location);
                break;
            }

            case Statement::Kind::Expression: {
                IRExpressionStatement expressionStatement;
                expressionStatement.expression = analyzeExpression(statement.expression, scope);

                append(std::move(expressionStatement), statement.location);
                break;
            }

            case Statement::Kind::Return: {
                if (!inFunction) {
                    error(statement.location, "ControlFlowError", "'return' can only appear inside a function");
                    break;
                }

                IRReturn returnStatement;
                returnStatement.type = returnType;

                if (returnType == ValueType::Unit) {
                    if (statement.expression) {
                        error(statement.location, "TypeError", "A Unit function cannot return a value");

                        returnStatement.expression = analyzeExpression(statement.expression, scope);
                    }
                } else if (!statement.expression) {
                    error(statement.location, "TypeError", "A function returning " + typeName(returnType) + " must return a value");
                } else {
                    returnStatement.expression = analyzeExpression(statement.expression, scope);
                    if (!canWiden(returnStatement.expression->type, returnType)) {
                        error(statement.expression->location, "TypeError", "Function returns " + typeName(returnStatement.expression->type) + "; declared return type is " + typeName(returnType));
                    }
                }

                append(std::move(returnStatement), statement.location);
                statementFlow.canFallThrough = false;
                statementFlow.mayReturn = true;
                break;
            }

            case Statement::Kind::If: {
                IRIf conditional;
                bool allBranchesReturn = true;
                bool hasBranch = false;

                for (const auto& branch: statement.branches) {
                    hasBranch = true;
                    auto condition = analyzeExpression(branch.condition, scope);
                    if (condition->type != ValueType::Boolean && condition->type != ValueType::Error) {
                        error(branch.location, "TypeError", "An if/elif condition must have type Boolean");
                    }
                    
                    Scope branchScope;
                    branchScope.parent = &scope;

                    IRBlock body;

                    if (branch.body) {
                        body = analyzeBlock(*branch.body, branchScope, inFunction, returnType, loopDepth);
                    }

                    allBranchesReturn = allBranchesReturn && !body.flow.canFallThrough;
                    conditional.branches.push_back(IRIfBranch{std::move(condition), std::make_shared<IRBlock>(std::move(body))});

                    statementFlow.mayReturn = statementFlow.mayReturn || conditional.branches.back().body->flow.mayReturn;
                    statementFlow.mayBreak = statementFlow.mayBreak || conditional.branches.back().body->flow.mayBreak;
                    statementFlow.mayContinue = statementFlow.mayContinue || conditional.branches.back().body->flow.mayContinue;
                }

                if (statement.elseBody) {
                    Scope elseScope;
                    elseScope.parent = &scope;

                    auto elseBody = analyzeBlock(*statement.elseBody, elseScope, inFunction, returnType, loopDepth);
                    allBranchesReturn = allBranchesReturn && !elseBody.flow.canFallThrough;
                    statementFlow.mayReturn = statementFlow.mayReturn || elseBody.flow.mayReturn;
                    statementFlow.mayBreak = statementFlow.mayBreak || elseBody.flow.mayBreak;
                    statementFlow.mayContinue = statementFlow.mayContinue || elseBody.flow.mayContinue;
                    conditional.elseBody = std::make_shared<IRBlock>(std::move(elseBody));
                } else {
                    allBranchesReturn = false;
                }

                statementFlow.canFallThrough = !hasBranch || !allBranchesReturn;
                append(std::move(conditional), statement.location);
                break;
            }

            case Statement::Kind::While: {
                auto condition = analyzeExpression(
                    statement.expression, scope
                );

                if (condition->type != ValueType::Boolean && condition->type != ValueType::Error) {
                    error(statement.location, "TypeError", "A while condition must have type Boolean");
                }

                Scope bodyScope;
                bodyScope.parent = &scope;

                IRBlock body;
                if (statement.body) {
                    body = analyzeBlock(*statement.body, bodyScope, inFunction, returnType, loopDepth+1);
                }

                IRWhile loop;
                loop.condition = std::move(condition);
                loop.body = std::make_shared<IRBlock>(std::move(body));

                statementFlow.canFallThrough = true;
                statementFlow.mayReturn = loop.body->flow.mayReturn;

                statementFlow.mayBreak = false;
                statementFlow.mayContinue = false;

                append(std::move(loop), statement.location);
                break;
            }

            case Statement::Kind::For: {
                if (!statement.expression || statement.expression->kind != Expr::Kind::Range || statement.expression->children.size() != 2) {
                    error(statement.location, "TypeError", "A for loop requires an integer range of the form start..end");
                    break;
                }

                auto start = analyzeExpression(statement.expression->children[0], scope);
                auto end = analyzeExpression(statement.expression->children[1], scope);

                if (start->type != ValueType::Integer && start->type != ValueType::Error) {
                    error(statement.expression->children[0]->location, "TypeError", "The starting value of a for range must be Integer");
                }
                if (end->type != ValueType::Integer && end->type != ValueType::Error) {
                    error(statement.expression->children[1]->location, "TypeError", "The ending value of a for range must be Integer");
                }

                Scope iteratorScope;
                iteratorScope.parent = &scope;

                if (isReservedName(statement.name) || signatures_.contains(statement.name)) {
                    error(statement.location, "NameError", "For-loop iterator name '" + statement.name + "' is reserved");
                }

                if (!allowShadowing_ && findSymbol(scope, statement.name)) {
                    error(statement.location, "NameError", "For-loop iterator '" + statement.name + "' shadows an existing name");
                }

                iteratorScope.symbols.emplace(statement.name, SymbolInfo{ValueType::Integer, false});

                Scope bodyScope;
                bodyScope.parent = &iteratorScope;

                IRBlock body;
                if (statement.body) {
                    body = analyzeBlock(*statement.body, bodyScope, inFunction, returnType, loopDepth + 1);
                }

                IRFor loop;
                loop.iterator = statement.name;
                loop.start = std::move(start);
                loop.end = std::move(end);
                loop.body = std::make_shared<IRBlock>(std::move(body));

                statementFlow.canFallThrough = true;
                statementFlow.mayReturn = loop.body->flow.mayReturn;
                statementFlow.mayBreak = false;
                statementFlow.mayContinue = false;

                append(std::move(loop), statement.location);
                break;
            }

            case Statement::Kind::Break: {
                if (loopDepth == 0) {
                    error(statement.location, "ControlFlowError", "'break' can only appear inside a loop");
                    break;
                }
                append(IRBreak{}, statement.location);
                statementFlow.canFallThrough = false;
                statementFlow.mayBreak = true;
                break;
            }

            case Statement::Kind::Continue: {
                if (loopDepth == 0) {
                    error(statement.location, "ControlFlowError", "'continue' can only appear inside a loop");
                    break;
                }
                append(IRContinue{}, statement.location);
                statementFlow.canFallThrough = false;
                statementFlow.mayContinue = true;
                break;
            }

            case Statement::Kind::Function:
                error(statement.location, "SemanticError", "Nested function definitions not supported");
                break;
        }

        mergeFlow(statementFlow);
    }
    result.flow.canFallThrough = reachable;
    return result;
}

void SemanticAnalyzer::analyzeFunction(const Statement& statement) {
    const auto signatureIt = signatures_.find(statement.name);
    if (signatureIt == signatures_.end()) return;
    Scope functionScope;
    functionScope.parent = &globals_;
    IRFunction function;
    function.name = statement.name;
    function.returnType = signatureIt->second.result;
    function.location = statement.location;

    for (std::size_t i = 0; i < statement.parameters.size(); ++i) {
        const auto& parameter = statement.parameters[i];
        const auto type = signatureIt->second.parameters[i];
        if (findSymbol(globals_, parameter.name) && !allowShadowing_)
            error(parameter.location, "RuntimeError", "Parameter '" + parameter.name + "' shadows an existing global name");
        functionScope.symbols.emplace(parameter.name, SymbolInfo{type, false});
        function.parameters.push_back(IRParameter{parameter.name, type});
    }

    AstBlock emptyBody;
    const AstBlock& body = statement.body ? *statement.body : emptyBody;
    function.body = analyzeBlock(body, functionScope, true, function.returnType, 0);
    if (function.returnType != ValueType::Unit && function.body.flow.canFallThrough) {
        error(statement.location, "ControlFlowError", "Function '" + statement.name + "' does not return a value on every possible execution path");
    }
    functions_[statement.name] = std::move(function);
}

SemanticResult SemanticAnalyzer::analyze(const AstProgram& ast) {
    diagnostics_.clear();
    signatures_.clear();
    functions_.clear();
    globals_ = Scope{};
    defaultDomain_ = ast.defaultDomain;

    static const std::unordered_set<std::string> domains{"Integer", "Rational", "Real", "Complex"};
    if (!domains.contains(defaultDomain_))
        error(ast.domainLocation, "DomainError", "Unknown default domain '" + defaultDomain_ +
              "'. Supported domains: Integer, Rational, Real, Complex");

    collectFunctions(ast);
    auto program = std::make_shared<ProgramIR>();
    program->defaultDomain = defaultDomain_;

    // Process top-level declarations in source order. Functions can call later
    // function declarations, but can only reference globals declared earlier.
    for (const auto& statement : ast.body.statements) {
        if (statement.kind == Statement::Kind::Function) {
            analyzeFunction(statement);
            continue;
        }
        AstBlock singleStatement;
        singleStatement.location = statement.location;
        singleStatement.statements.push_back(statement);

        IRBlock lowered = analyzeBlock(singleStatement, globals_, false,
                                         ValueType::Error, 0);
        program->body.statements.insert(program->body.statements.end(), std::make_move_iterator(lowered.statements.begin()), std::make_move_iterator(lowered.statements.end()));
    }
    program->functions = functions_;

    if (!diagnostics_.empty()) program.reset();
    return SemanticResult{std::move(program), std::move(diagnostics_)};
}

} // namespace cmath::language::detail
