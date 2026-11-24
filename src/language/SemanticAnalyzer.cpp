#include "LanguageInternal.h"

#include <cmath>

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <utility>
#include <iterator>
#include <type_traits>

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

std::string typeName(const LanguageType& type) {
    if (type.kind == LanguageType::Kind::Scalar) {
        return typeName(type.scalar);
    }

    if (type.kind == LanguageType::Kind::TypeVariable) {
        return "'T" + std::to_string(type.variableId);
    }

    std::string result = "fn(";
    
    for (std::size_t i = 0; i < type.parameters.size(); ++i) {
        if (i != 0) result += ", ";
        result += typeName(type.parameters[i]);
    }

    result += ") -> ";
    result += type.result ? typeName(*type.result) : "<error>";
    return result;
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

LanguageType SemanticAnalyzer::freshTypeVariable() {
    return LanguageType::typeVariable(nextTypeVariableId_++);
}

LanguageType SemanticAnalyzer::resolveTypeVariables(const LanguageType& type) const {
    if (type.kind == LanguageType::Kind::TypeVariable) {
        const auto found = typeSubstitutions_.find(type.variableId);

        if (found == typeSubstitutions_.end()) return type;

        return resolveTypeVariables(found->second);
    }

    if (type.kind == LanguageType::Kind::Scalar) return type;

    std::vector<LanguageType> parameters;
    parameters.reserve(type.parameters.size());

    for (const auto& parameter : type.parameters) {
        parameters.push_back(resolveTypeVariables(parameter));
    }

    LanguageType resultType = type.result ? resolveTypeVariables(*type.result) : LanguageType::scalarType(ValueType::Error);
    return LanguageType::functionType(std::move(parameters), std::move(resultType));
}

bool SemanticAnalyzer::occursInType(std::size_t variableId, const LanguageType& originalType) const {
    const LanguageType type = resolveTypeVariables(originalType);
    if (type.kind == LanguageType::Kind::TypeVariable) {
        return type.variableId == variableId;
    }

    if (type.kind == LanguageType::Kind::Scalar) return false;

    for (const auto& parameter : type.parameters) {
        if (occursInType(variableId, parameter)) return true;
    }

    return type.result && occursInType(variableId, *type.result);
}

bool SemanticAnalyzer::unifyTypes(const LanguageType& left, const LanguageType& right, const SourceLocation& location, const std::string& context) {
    if (left.kind == LanguageType::Kind::TypeVariable) {
        const auto found = typeSubstitutions_.find(left.variableId);
        const LanguageType target = resolveTypeVariables(right);

        if (target.kind == LanguageType::Kind::TypeVariable && target.variableId == left.variableId) return true;

        if (found == typeSubstitutions_.end()) {
            if (occursInType(left.variableId, target)) {
                error(location, "TypeInferenceError", "Infinite type detected while inferring " + context);
                return false;
            }

            typeSubstitutions_[left.variableId] = target;
            return true;
        }

        const LanguageType current = resolveTypeVariables(found->second);
        if (current == target) return true;

        if (current.kind == LanguageType::Kind::Scalar && target.kind == LanguageType::Kind::Scalar && isNumeric(current.scalar) && isNumeric(target.scalar)) {
            typeSubstitutions_[left.variableId] = LanguageType::scalarType(promotedNumeric(current.scalar, target.scalar));
            return true;
        }

        return unifyTypes(current, target, location, context);
    }

    if (right.kind == LanguageType::Kind::TypeVariable) {
        return unifyTypes(right, left, location, context);
    }

    const LanguageType resolvedLeft = resolveTypeVariables(left);
    const LanguageType resolvedRight = resolveTypeVariables(right);

    if (resolvedLeft == resolvedRight) return true;

    if (resolvedLeft.kind == LanguageType::Kind::Function && resolvedRight.kind == LanguageType::Kind::Function) {
        if (resolvedLeft.parameters.size() != resolvedRight.parameters.size()) {
            error(location, "TypeInferenceError", "Cannot unify incompatible function signatures while inferring " + context);
            return false;
        }

        bool succeeded = true;
        for (std::size_t i = 0; i < resolvedLeft.parameters.size(); ++i) {
            succeeded = unifyTypes(resolvedLeft.parameters[i], resolvedRight.parameters[i], location, context) && succeeded;
        }

        if (resolvedLeft.result && resolvedRight.result) {
            succeeded = unifyTypes(*resolvedLeft.result, *resolvedRight.result, location, context) && succeeded;
        }

        return succeeded;
    }

    error(location, "TypeInferenceError", "Cannot infer a consistent type for " + context + ": receiveed " + typeName(resolvedLeft) + " and " + typeName(resolvedRight));
    return false;
}

bool SemanticAnalyzer::containsTypeVariable(const LanguageType& originalType) const {
    const LanguageType type = resolveTypeVariables(originalType);
    if (type.kind == LanguageType::Kind::TypeVariable) return true;
    if (type.kind == LanguageType::Kind::Scalar) return false;

    for (const auto& parameter : type.parameters) {
        if (containsTypeVariable(parameter)) return true;
    }

    return type.result && containsTypeVariable(*type.result);
}

LanguageType SemanticAnalyzer::instantiateTypeScheme(const LanguageType& originalType, const std::vector<std::size_t>& quantified) {
    const std::unordered_set<std::size_t> quantifiedSet(quantified.begin(), quantified.end());
    std::unordered_map<std::size_t, LanguageType> replacements;
    
    std::function<LanguageType(const LanguageType&)> clone = 
        [&](const LanguageType& original) -> LanguageType {
            const LanguageType type = resolveTypeVariables(original);
            if (type.kind == LanguageType::Kind::Scalar) return type;
            if (type.kind == LanguageType::Kind::TypeVariable) {
                if (!quantifiedSet.contains(type.variableId)) return type;
                const auto found = replacements.find(type.variableId);
                if (found != replacements.end()) return found->second;

                LanguageType fresh = freshTypeVariable();
                replacements.emplace(type.variableId, fresh);
                return fresh;
            }

            std::vector<LanguageType> parameters;
            parameters.reserve(type.parameters.size());

            for (const auto& parameter : type.parameters) {
                parameters.push_back(clone(parameter));
            }

            LanguageType result = type.result ? clone(*type.result) : LanguageType::scalarType(ValueType::Error);
            
            return LanguageType::functionType(std::move(parameters), std::move(result));
        };

    return clone(originalType);
}

std::vector<std::size_t> SemanticAnalyzer::generalizeType(const LanguageType& originalType, const Scope& scope) const {
    using VariableSet = std::unordered_set<std::size_t>;
    auto collect = [this](const LanguageType& root, VariableSet& variables) {
        std::function<void(const LanguageType&)> visit =
        [&](const LanguageType& original) {
            const LanguageType type = resolveTypeVariables(original);
            if (type.kind == LanguageType::Kind::TypeVariable) {
                variables.insert(type.variableId);
                return;
            }

            if (type.kind == LanguageType::Kind::Function) {
                for (const auto& parameter : type.parameters) {
                    visit(parameter);
                }

                if (type.result) visit(*type.result);
            }
        };

        visit(root);
    };

    VariableSet candidates;
    collect(originalType, candidates);

    VariableSet environmentVariables;

    for (const Scope* current = &scope; current != nullptr; current = current->parent) {
        for (const auto& [name, symbol] : current->symbols) {
            (void) name;

            VariableSet symbolVariables;
            collect(symbol.typeInfo, symbolVariables);

            for (const auto quantified : symbol.quantifiedTypeVariables) {
                symbolVariables.erase(quantified);
            }

            environmentVariables.insert(symbolVariables.begin(), symbolVariables.end());
        }
    }

    for (const auto variable : environmentVariables) {
        candidates.erase(variable);
    }

    std::vector<std::size_t> result(candidates.begin(), candidates.end());
    std::sort(result.begin(), result.end());
    return result;
}

bool SemanticAnalyzer::isNonExpansive(const ExprPtr& expression) const {
    if (!expression) return false;
    switch (expression->kind) {
        case Expr::Kind::Number:
        case Expr::Kind::String:
        case Expr::Kind::Boolean:
        case Expr::Kind::Identifier:
            return true;
        default:
            return false;
    }
}

bool SemanticAnalyzer::compatibleTypes(const LanguageType& actualOriginal, const LanguageType& expectedOriginal, const SourceLocation& location, const std::string& context) {
    const LanguageType actual = resolveTypeVariables(actualOriginal);
    const LanguageType expected = resolveTypeVariables(expectedOriginal);
    if (containsTypeVariable(actual) || containsTypeVariable(expected)) {
        return unifyTypes(actual, expected, location, context);
    }

    if (actual.kind == LanguageType::Kind::Scalar && expected.kind == LanguageType::Kind::Scalar) {
        return canWiden(actual.scalar, expected.scalar);
    }

    if (actual == expected) {
        return true;
    }

    return unifyTypes(actual, expected, location, context);
}

std::optional<ValueType> SemanticAnalyzer::resolveType(const std::string& name,
                                                        const SourceLocation& loc) {
    auto type = parseTypeName(name);
    if (!type) error(loc, "TypeError", "Unknown type '" + name + "'");
    return type;
}

std::optional<LanguageType> SemanticAnalyzer::resolveType(const TypeSyntax& syntax, const SourceLocation& loc) {
    if (syntax.kind == TypeSyntax::Kind::Named) {
        auto scalar = parseTypeName(syntax.name);

        if (!scalar) {
            error(syntax.location, "TypeError", "Unknown type '" + syntax.name + "'");
            return std::nullopt;
        }
        return LanguageType::scalarType(*scalar);
    }

    std::vector<LanguageType> parameters;

    for (const auto& parameter : syntax.parameters) {
        auto resolved = resolveType(parameter, loc);
        if (!resolved) return std::nullopt;
        parameters.push_back(std::move(*resolved));
    }

    if (!syntax.result) {
        error(loc, "TypeError", "Function type is missing its return type");
        return std::nullopt;
    }

    auto result = resolveType(*syntax.result, loc);

    if (!result) return std::nullopt;

    return LanguageType::functionType(std::move(parameters), std::move(*result));
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
    result->typeInfo = LanguageType::scalarType(type);
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
            TypeSyntax syntax = parameter.typeSyntax;

            if (syntax.kind == TypeSyntax::Kind::Named && syntax.name.empty()) {
                signature.parameterTypes.push_back(freshTypeVariable());
                signature.parameters.push_back(ValueType::Error);
                continue;
            }

            if (syntax.kind == TypeSyntax::Kind::Named && syntax.name.empty()) {
                syntax.name = parameter.typeName;
                syntax.location = parameter.location;
            }

            auto type = resolveType(syntax, parameter.location);
            if (!type) {
                valid = false;
                continue;
            }
            
            signature.parameters.push_back(type->kind == LanguageType::Kind::Scalar ? type->scalar : ValueType::Error);
            signature.parameterTypes.push_back(std::move(*type));
        }

        if (statement.inferReturnType) {
            signature.result = ValueType::Error;
            signature.resultType = freshTypeVariable();
        } else {
            TypeSyntax returnSyntax = statement.returnTypeSyntax;

            if (returnSyntax.kind == TypeSyntax::Kind::Named && returnSyntax.name.empty()) {
                returnSyntax.name = statement.returnType;
                returnSyntax.location = statement.location;
            }

            auto returnType = resolveType(returnSyntax, statement.location);

            if (!returnType) valid = false;
            else {
                signature.result = returnType->kind == LanguageType::Kind::Scalar ? returnType->scalar : ValueType::Error;
                signature.resultType = std::move(*returnType);
            }
        }

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
            if (symbol) {
                LanguageType instantiated = instantiateTypeScheme(symbol->typeInfo, symbol->quantifiedTypeVariables);
                const ValueType scalar = instantiated.kind == LanguageType::Kind::Scalar ? instantiated.scalar : ValueType::Error;
                auto result = makeIR(IRExpression::Kind::Variable, expression->text, scalar, expression->location);
                result->typeInfo = std::move(instantiated);
                return result;
            }
            const auto function = signatures_.find(expression->text);
            if (function != signatures_.end()) {
                std::vector<LanguageType> parameterTypes;
                parameterTypes.reserve(function->second.parameterTypes.size());

                for (const auto& parameterType : function->second.parameterTypes) {
                    parameterTypes.push_back(parameterType);
                }

                LanguageType functionType = LanguageType::functionType(std::move(parameterTypes), function->second.resultType);
                if (!analyzingRecursiveGroup_) {
                    functionType = instantiateTypeScheme(functionType, function->second.quantifiedTypeVariables);
                }
                auto result = makeIR(IRExpression::Kind::Variable, expression->text, ValueType::Error, expression->location);
                result->typeInfo = std::move(functionType);
                return result;
            }
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
            const ValueType defaultNumeric = parseTypeName(defaultDomain_).value_or(ValueType::Integer);
            auto refresh = [this](const IRExprPtr& value) {
                value->typeInfo = resolveTypeVariables(value->typeInfo);
                value->type = value->typeInfo.kind == LanguageType::Kind::Scalar ? value->typeInfo.scalar : ValueType::Error;
            };

            auto constrainNumericPair = [&](bool integerOnly = false) {
                LanguageType leftType = resolveTypeVariables(left->typeInfo);
                LanguageType rightType = resolveTypeVariables(right->typeInfo);

                if (op == "+" && 
                    leftType.kind == LanguageType::Kind::TypeVariable && 
                    rightType.kind == LanguageType::Kind::Scalar &&
                    rightType.scalar == ValueType::String) {
                        unifyTypes(leftType, LanguageType::scalarType(ValueType::String), expression->location, "string concatenation");
                    }

                if (op == "+" &&
                    leftType.kind == LanguageType::Kind::TypeVariable &&
                    rightType.kind == LanguageType::Kind::Scalar &&
                    leftType.scalar == ValueType::String) {
                        unifyTypes(rightType, LanguageType::scalarType(ValueType::String), expression->location, "string concatenation");
                    }

                leftType = resolveTypeVariables(left->typeInfo);
                rightType = resolveTypeVariables(right->typeInfo);

                if (leftType.kind == LanguageType::Kind::TypeVariable &&
                    rightType.kind == LanguageType::Kind::TypeVariable) {
                        unifyTypes(leftType, rightType, expression->location, "numeric operands");
                    }
                
                leftType = resolveTypeVariables(left->typeInfo);
                rightType = resolveTypeVariables(right->typeInfo);

                if (leftType.kind == LanguageType::Kind::TypeVariable &&
                    rightType.kind == LanguageType::Kind::Scalar &&
                    isNumeric(rightType.scalar)) {
                        unifyTypes(leftType, rightType, expression->location, "numeric operand");
                    }

                if (rightType.kind == LanguageType::Kind::TypeVariable &&
                    leftType.kind == LanguageType::Kind::Scalar &&
                    isNumeric(leftType.scalar)) {
                        unifyTypes(rightType, leftType, expression->location, "numeric operands");
                    }

                const ValueType fallback = integerOnly ? ValueType::Error : defaultNumeric;

                leftType = resolveTypeVariables(left->typeInfo);
                rightType = resolveTypeVariables(right->typeInfo);

                if (leftType.kind == LanguageType::Kind::TypeVariable) {
                    unifyTypes(leftType, LanguageType::scalarType(fallback), expression->location, "numeric operand");
                }

                if (rightType.kind == LanguageType::Kind::TypeVariable) {
                    unifyTypes(rightType, LanguageType::scalarType(fallback), expression->location, "numeric operand");
                }

                refresh(left);
                refresh(right);
            };

            if (op == "+" || op == "-" || op == "*" ||
                op == "/" || op == "^" || op == "%") {
                    constrainNumericPair(op == "%");
            } else if (op == "and" || op == "or") {
                for (auto& operand : {left, right}) {
                    const LanguageType type = resolveTypeVariables(operand->typeInfo);

                    if (type.kind == LanguageType::Kind::TypeVariable) {
                        unifyTypes(type, LanguageType::scalarType(ValueType::Boolean), expression->location, "logical operand");
                    }

                    refresh(operand);
                }
            } else if (op == "==" || op == "!=") {
                const LanguageType leftType = resolveTypeVariables(left->typeInfo);
                const LanguageType rightType = resolveTypeVariables(right->typeInfo);

                if (leftType.kind == LanguageType::Kind::TypeVariable || rightType.kind == LanguageType::Kind::TypeVariable) {
                    unifyTypes(leftType, rightType, expression->location, "equality operands");
                }

                refresh(left);
                refresh(right);
            } else {
                constrainNumericPair();
            }

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
                    if (right->type == ValueType::Complex || left->type == ValueType::Complex) {
                        result = ValueType::Complex;
                    } else if (right->type == ValueType::Integer) {
                        if (left->type == ValueType::Integer) {
                            const auto knownExponent = constantReal(expression->children[1]);
                            result = knownExponent && *knownExponent >= 0.0L && std::floor(*knownExponent) == *knownExponent ? ValueType::Integer : ValueType::Rational;
                        } else {
                            result = left->type;
                        }
                    } else {
                        result = ValueType::Real;
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

    auto compatible = [this](const LanguageType& actual, const LanguageType& expected, const SourceLocation& location, const std::string& context) {
        return compatibleTypes(actual, expected, location, context);
    };

    if (const auto* symbol = findSymbol(scope, name)) {
        auto callee = analyzeExpression(expression->children.front(), scope);
        LanguageType signature = resolveTypeVariables(callee->typeInfo);
        
        if (signature.kind == LanguageType::Kind::TypeVariable) {
            std::vector<LanguageType> parameterTypes;
            parameterTypes.reserve(arguments.size());

            for (std::size_t i = 0; i < arguments.size(); ++i) {
                parameterTypes.push_back(freshTypeVariable());
            }

            LanguageType returnType = freshTypeVariable();
            const LanguageType inferredFunction = LanguageType::functionType(std::move(parameterTypes), returnType);
            unifyTypes(signature, inferredFunction, expression->location, "function parameter '" + name + "'");
            signature = resolveTypeVariables(callee->typeInfo);
        }

        if (signature.kind != LanguageType::Kind::Function || !signature.result) {
            error(expression->location, "TypeError", "Value '" + name + "' is not callable");
            result = makeIR(IRExpression::Kind::Call, name, ValueType::Error, expression->location);
            return ValueType::Error;
        }

        if (arguments.size() != signature.parameters.size()) {
            error(expression->location, "TypeError", "Function value '" + name + "' expects " + std::to_string(signature.parameters.size()) + " argument(s), but received " + std::to_string(arguments.size()));
        } else {
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                if (!compatible(arguments[i]->typeInfo, signature.parameters[i], arguments[i]->location, "argument to '" + name + "'")) {
                    error(arguments[i]->location, "TypeError", "Argument " + std::to_string(i+1) + " of '" + name + "' has type " + typeName(arguments[i]->typeInfo) + "; expected " + typeName(signature.parameters[i]));
                }
            }
        }

        std::vector<IRExprPtr> callChildren;

        callChildren.push_back(std::move(callee));
        callChildren.insert(callChildren.end(), arguments.begin(), arguments.end());
        const LanguageType resultInfo = resolveTypeVariables(*signature.result);
        const ValueType resultType = resultInfo.kind == LanguageType::Kind::Scalar ? resultInfo.scalar : ValueType::Error;

        result = makeIR(IRExpression::Kind::Call, name, resultType, expression->location, std::move(callChildren));
        result->typeInfo = resultInfo;
        result->indirectCall = true;

        return resultType;
    }

    if (const auto function = signatures_.find(name); function != signatures_.end()) {
        LanguageType signature = LanguageType::functionType(function->second.parameterTypes, function->second.resultType);
        if (!analyzingRecursiveGroup_) {
            signature = instantiateTypeScheme(signature, function->second.quantifiedTypeVariables);
        }

        signature = resolveTypeVariables(signature);
        
        if (arguments.size() != function->second.parameters.size()) {
            error(expression->location, "RuntimeError", "Function '" + name + "' received the wrong number of arguments");
        } else {
            for (std::size_t i = 0; i < arguments.size(); i++) {
                if (!compatible(arguments[i]->typeInfo, signature.parameters[i], arguments[i]->location, "argument to '" + name + "'")) {
                    error(arguments[i]->location, "TypeError", "Argument " + std::to_string(i + 1) + " of '" + name + "' has type " + typeName(arguments[i]->typeInfo) +"; expected " + typeName(signature.parameters[i]));
                }
            }
        }

        const LanguageType resultInfo = signature.result ? resolveTypeVariables(*signature.result) : LanguageType::scalarType(ValueType::Error);
        const ValueType resultType = resultInfo.kind == LanguageType::Kind::Scalar ? resultInfo.scalar : ValueType::Error;

        result = makeIR(IRExpression::Kind::Call, name, resultType, expression->location, std::move(arguments));
        result->typeInfo = resultInfo;
        return function->second.result;
    }

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
    if (name == "print") {
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
    result->typeInfo = LanguageType::scalarType(returnType);
    return returnType;
}

IRBlock SemanticAnalyzer::analyzeBlock(const AstBlock& block, Scope& scope, bool inFunction, LanguageType returnType, std::size_t loopDepth) {
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
                LanguageType declaredInfo = initializer->typeInfo;
                const bool hasExplicitType = !statement.typeName.empty() || statement.typeSyntax.kind == TypeSyntax::Kind::Function || !statement.typeSyntax.name.empty();

                if (hasExplicitType) {
                    TypeSyntax syntax = statement.typeSyntax;
                    if (syntax.kind == TypeSyntax::Kind::Named && syntax.name.empty()) {
                        syntax.name = statement.typeName;
                        syntax.location = statement.location;
                    }

                    auto explicitType = resolveType(syntax, statement.location);
                    if (explicitType) {
                        if (!compatibleTypes(initializer->typeInfo, *explicitType, statement.location, "variable initializer")) {
                            error(statement.expression ? statement.expression->location : statement.location, "TypeError", "Cannot assign " + typeName(initializer->typeInfo) + " to " + typeName(*explicitType));
                        }

                        declaredInfo = *explicitType;
                        declared = explicitType->kind == LanguageType::Kind::Scalar ? explicitType->scalar : ValueType::Error;
                    }
                }

                std::vector<std::size_t> quantified;

                if (!statement.mutableBinding && isNonExpansive(statement.expression)) {
                    quantified = generalizeType(declaredInfo, scope);
                }

                if (!scope.symbols.contains(statement.name)) {
                    scope.symbols.emplace(statement.name, SymbolInfo{declared, statement.mutableBinding, declaredInfo, std::move(quantified)});
                }

                IRDeclaration declaration;
                declaration.name = statement.name;
                declaration.type = declared;
                declaration.mutableBinding = statement.mutableBinding;
                declaration.initializer = std::move(initializer);
                declaration.typeInfo = declaredInfo;

                append(std::move(declaration), statement.location);
                break;
            }
            case Statement::Kind::Lambda: {
                bool valid = true;

                if (isReservedName(statement.name) || signatures_.contains(statement.name)) {
                    error(statement.location, "SemanticError", "Lambda name '" + statement.name + "' is reserved");
                    valid = false;
                }

                if (scope.symbols.contains(statement.name)) {
                    error(statement.location, "NameError", "Name '" + statement.name + "' is already declared in this scope");
                    valid = false;
                } else if (!allowShadowing_ && scope.parent && findSymbol(*scope.parent, statement.name)) {
                    error(statement.location, "NameError", "Lambda declaration '" + statement.name + "' shadows an existing name");
                    valid = false;
                }

                auto function = std::make_shared<IRFunction>();
                function->name = statement.name;
                function->location = statement.location;

                Scope functionScope;
                functionScope.parent = &scope;

                std::unordered_set<std::string> parameterNames;
                std::vector<LanguageType> parameterTypes;

                for (const auto& parameter : statement.parameters) {
                    if (isReservedName(parameter.name)) {
                        error(parameter.location, "SemanticError", "Parameter name '" + parameter.name + "' is reserved");
                        valid = false;
                    }

                    if (!parameterNames.insert(parameter.name).second) {
                        error(parameter.location, "SemanticError", "Duplicate lambda paramter '" + parameter.name + "'");
                        valid = false;
                    }

                    TypeSyntax syntax = parameter.typeSyntax;
                    LanguageType resolvedType;

                    if (syntax.kind == TypeSyntax::Kind::Named && syntax.name.empty()) {
                        resolvedType = freshTypeVariable();
                    } else {
                        if (syntax.kind == TypeSyntax::Kind::Named && syntax.name.empty()) {
                            syntax.name = parameter.typeName;
                            syntax.location = parameter.location;
                        }
                        auto resolved = resolveType(syntax, parameter.location);
                        if (!resolved) {
                            valid = false; 
                            continue;
                        }

                        resolvedType = std::move(*resolved);
                    }

                    const ValueType scalarType = resolvedType.kind == LanguageType::Kind::Scalar ? resolvedType.scalar : ValueType::Error;
                    parameterTypes.push_back(resolvedType);

                    function->parameters.push_back(IRParameter{parameter.name, scalarType, resolvedType});
                    functionScope.symbols.emplace(parameter.name, SymbolInfo{scalarType, false, resolvedType});
                }

                LanguageType lambdaReturnType = statement.inferReturnType ? freshTypeVariable() : LanguageType::scalarType(ValueType::Error);

                if (!statement.inferReturnType) {
                    TypeSyntax returnSyntax = statement.returnTypeSyntax;
                    if (returnSyntax.kind == TypeSyntax::Kind::Named && returnSyntax.name.empty()) {
                        returnSyntax.name = statement.returnType;
                        returnSyntax.location = statement.location;
                    }

                    auto resolvedReturn = resolveType(returnSyntax, statement.location);

                    if (resolvedReturn) {
                        lambdaReturnType = *resolvedReturn;
                    } else {
                        valid = false;
                    }
                }

                function->returnTypeInfo = lambdaReturnType;

                AstBlock emptyBody;
                const AstBlock& lambdaBody = statement.body ? *statement.body : emptyBody;
                function->body = analyzeBlock(lambdaBody, functionScope, true, lambdaReturnType, 0);

                if (statement.inferReturnType) {
                    if (function->body.flow.canFallThrough) {
                        if (!function->body.flow.mayReturn) {
                            unifyTypes(lambdaReturnType, LanguageType::scalarType(ValueType::Unit), statement.location, "lambda return type");
                        } else {
                            error(statement.location, "ControlFlowError", "Lambda '" + statement.name + "' does not return a value on every possible execution path");
                            valid = false;
                        }
                    }
                } else {
                    const bool returnsUnit = lambdaReturnType.kind == LanguageType::Kind::Scalar && lambdaReturnType.scalar == ValueType::Unit;

                    if (!returnsUnit && function->body.flow.canFallThrough) {
                        error(statement.location, "ControlFlowError", "Lambda '" + statement.name + "' does not return a value on every possible execution path");
                        valid = false;
                    }
                }

                LanguageType lambdaType = LanguageType::functionType(std::move(parameterTypes), lambdaReturnType);
                std::vector<std::size_t> quantified;

                if (!statement.mutableBinding) {
                    quantified = generalizeType(lambdaType, scope);
                }

                function->quantifiedTypeVariables = quantified;

                if (valid) {
                    scope.symbols.emplace(
                        statement.name, SymbolInfo{ValueType::Error, statement.mutableBinding, lambdaType, std::move(quantified)}
                    );
                }

                IRDeclaration declaration;
                declaration.name = statement.name;
                declaration.type = ValueType::Error;
                declaration.typeInfo = std::move(lambdaType);
                declaration.mutableBinding = statement.mutableBinding;
                declaration.lambdaFunction = std::move(function);

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

                const bool compatible = compatibleTypes(value->typeInfo, symbol->typeInfo, statement.location, "assigned to '" + statement.name + "'");

                if (!compatible) {
                    error(statement.location, "TypeError", "Cannot assign " + typeName(value->typeInfo) + " to variable '" + statement.name + "' of type " + typeName(symbol->typeInfo));
                }

                IRAssignment assignment;
                assignment.name = statement.name;
                assignment.type = symbol->type;
                assignment.expression = std::move(value);
                assignment.typeInfo = symbol->typeInfo;

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
                returnStatement.type = returnType.kind == LanguageType::Kind::Scalar ? returnType.scalar : ValueType::Error;
                returnStatement.typeInfo = returnType;

                if (returnType.isTypeVariable()) {
                    if (!statement.expression) {
                        unifyTypes(returnType, LanguageType::scalarType(ValueType::Unit), statement.location, "function return type");
                    } else {
                        returnStatement.expression = analyzeExpression(statement.expression, scope);
                        unifyTypes(returnType, returnStatement.expression->typeInfo, statement.location, "function return type");
                    }
                } else if (returnType.kind == LanguageType::Kind::Scalar && returnType.scalar == ValueType::Unit) {
                    if (statement.expression) {
                        error(statement.location, "TypeError", "A Unit function cannot return a value");

                        returnStatement.expression = analyzeExpression(statement.expression, scope);
                    }
                } else if (!statement.expression) {
                    error(statement.location, "TypeError", "A function returning " + typeName(returnType) + " must return a value");
                } else {
                    returnStatement.expression = analyzeExpression(statement.expression, scope);
                    const bool compatible = compatibleTypes(returnStatement.expression->typeInfo, returnType, statement.expression->location, "function return value");
                    if (!compatible) {
                        error(statement.expression->location, "TypeError", "Function returns " + typeName(returnStatement.expression->typeInfo) + "; declared return type is " + typeName(returnType));
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
    function.returnTypeInfo = signatureIt->second.resultType;
    function.location = statement.location;

    for (std::size_t i = 0; i < statement.parameters.size(); ++i) {
        const auto& parameter = statement.parameters[i];
        const auto type = signatureIt->second.parameters[i];
        const auto& typeInfo = signatureIt->second.parameterTypes[i];
        if (findSymbol(globals_, parameter.name) && !allowShadowing_)
            error(parameter.location, "RuntimeError", "Parameter '" + parameter.name + "' shadows an existing global name");
        functionScope.symbols.emplace(parameter.name, SymbolInfo{type, false, typeInfo});
        function.parameters.push_back(IRParameter{parameter.name, type, typeInfo});
    }

    AstBlock emptyBody;
    const AstBlock& body = statement.body ? *statement.body : emptyBody;
    function.body = analyzeBlock(body, functionScope, true, function.returnTypeInfo, 0);
    
    if (statement.inferReturnType) {
        if (function.body.flow.canFallThrough) {
            if (!function.body.flow.mayReturn) {
                unifyTypes(function.returnTypeInfo, LanguageType::scalarType(ValueType::Unit), statement.location, "function return type");
            } else {
                error(statement.location, "ControlFlowError" , "function '" + statement.name + "' does not return a value on every possible execution path");
            }
        }
    } else if (function.returnType != ValueType::Unit && function.body.flow.canFallThrough) {
        error(statement.location, "ControlFlowError", "Function '" + statement.name + "' does not return a value on every possible execution path");
    }
    
    functions_[statement.name] = std::move(function);
}

SemanticResult SemanticAnalyzer::analyze(const AstProgram& ast) {
    diagnostics_.clear();
    signatures_.clear();
    functions_.clear();
    globals_ = Scope{};
    typeSubstitutions_.clear();
    nextTypeVariableId_ = 0;
    defaultDomain_ = ast.defaultDomain;

    static const std::unordered_set<std::string> domains{"Integer", "Rational", "Real", "Complex"};
    if (!domains.contains(defaultDomain_))
        error(ast.domainLocation, "DomainError", "Unknown default domain '" + defaultDomain_ +
              "'. Supported domains: Integer, Rational, Real, Complex");

    collectFunctions(ast);
    const std::size_t functionCount = static_cast<std::size_t>(
        std::count_if(ast.body.statements.begin(), ast.body.statements.end(), [](const Statement& statement) {return statement.kind == Statement::Kind::Function;})
    );
    std::size_t analyzedFunctionCount = 0;
    analyzingRecursiveGroup_ = functionCount != 0;
    auto finishRecursiveGroup = [&]() {
        for (auto& [name, signature] : signatures_) {
            (void)name;
            LanguageType functionType = LanguageType::functionType(signature.parameterTypes, signature.resultType);
            signature.quantifiedTypeVariables = generalizeType(functionType, globals_);
            const auto function = functions_.find(name);
            if (function != functions_.end()) {
                function->second.quantifiedTypeVariables = signature.quantifiedTypeVariables;
            }
        }
        analyzingRecursiveGroup_ = false;
    };
    auto program = std::make_shared<ProgramIR>();
    program->defaultDomain = defaultDomain_;

    // Process top-level declarations in source order. Functions can call later
    // function declarations, but can only reference globals declared earlier.
    for (const auto& statement : ast.body.statements) {
        if (statement.kind == Statement::Kind::Function) {
            analyzeFunction(statement);
            ++analyzedFunctionCount;
            if (analyzedFunctionCount == functionCount) {
                finishRecursiveGroup();
            }
            continue;
        }
        AstBlock singleStatement;
        singleStatement.location = statement.location;
        singleStatement.statements.push_back(statement);

        IRBlock lowered = analyzeBlock(singleStatement, globals_, false,
                                        LanguageType::scalarType(ValueType::Error), 0);
        program->body.statements.insert(program->body.statements.end(), std::make_move_iterator(lowered.statements.begin()), std::make_move_iterator(lowered.statements.end()));
    }

    for (auto& [name, function] : functions_) {
        (void)name;
        if (analyzingRecursiveGroup_) {
            finishRecursiveGroup();
        }
        resolveIRFunctionTypes(function);
    }

    resolveIRBlockTypes(program->body);

    program->functions = functions_;

    if (!diagnostics_.empty()) program.reset();
    return SemanticResult{std::move(program), std::move(diagnostics_)};
}

void SemanticAnalyzer::resolveIRExpressionTypes(IRExprPtr& expression) {
    if (!expression) {return;}

    for (auto& child : expression->children) {
        resolveIRExpressionTypes(child);
    }

    expression->typeInfo = resolveTypeVariables(expression->typeInfo);
    expression->type = expression->typeInfo.kind == LanguageType::Kind::Scalar ? expression->typeInfo.scalar : ValueType::Error;
}

void SemanticAnalyzer::resolveIRBlockTypes(IRBlock& block) {
    for (auto& statement : block.statements) {
        std::visit(
            [this](auto& node) {
                using T = std::decay_t<decltype(node)>;

                if constexpr(std::is_same_v<T, IRDeclaration>) {
                    node.typeInfo = resolveTypeVariables(node.typeInfo);
                    node.type = node.typeInfo.kind == LanguageType::Kind::Scalar ? node.typeInfo.scalar : ValueType::Error;
                    resolveIRExpressionTypes(node.initializer);

                    if (node.lambdaFunction) {
                        resolveIRFunctionTypes(*node.lambdaFunction);
                    }
                } else if constexpr(std::is_same_v<T, IRAssignment>) {
                    node.typeInfo = resolveTypeVariables(node.typeInfo);
                    node.type = node.typeInfo.kind == LanguageType::Kind::Scalar ? node.typeInfo.scalar : ValueType::Error;
                    resolveIRExpressionTypes(node.expression);
                } else if constexpr(std::is_same_v<T, IRExpressionStatement>) resolveIRExpressionTypes(node.expression);
                else if constexpr(std::is_same_v<T, IRReturn>) {
                    node.typeInfo = resolveTypeVariables(node.typeInfo);
                    node.type = node.typeInfo.kind == LanguageType::Kind::Scalar ? node.typeInfo.scalar : ValueType::Error;
                    resolveIRExpressionTypes(node.expression);
                } else if constexpr(std::is_same_v<T, IRIf>) {
                    for (auto& branch : node.branches) {
                        resolveIRExpressionTypes(branch.condition);
                        if (branch.body) resolveIRBlockTypes(*branch.body);
                    }

                    if (node.elseBody) {
                        resolveIRBlockTypes(*node.elseBody);
                    }
                } else if constexpr(std::is_same_v<T, IRWhile>) {
                    resolveIRExpressionTypes(node.condition);
                    if (node.body) resolveIRBlockTypes(*node.body);
                } else if constexpr(std::is_same_v<T, IRFor>) {
                    resolveIRExpressionTypes(node.start);
                    resolveIRExpressionTypes(node.end);
                    if (node.body) resolveIRBlockTypes(*node.body);
                }
            }, 
            statement.data
        );
    }
}

void SemanticAnalyzer::resolveIRFunctionTypes(IRFunction& function) {
    const std::unordered_set<std::size_t> quantified(function.quantifiedTypeVariables.begin(), function.quantifiedTypeVariables.end());
    auto hasUnquantifiedTypeVariable =
        [this, &quantified](const LanguageType& original) {
            const LanguageType type = resolveTypeVariables(original);

            auto visit = [&](const auto& self,
                             const LanguageType& current) -> bool {
                if (current.kind == LanguageType::Kind::TypeVariable) {
                    return !quantified.contains(current.variableId);
                }

                if (current.kind == LanguageType::Kind::Scalar) {
                    return false;
                }

                for (const auto& parameter : current.parameters) {
                    if (self(self, parameter)) {
                        return true;
                    }
                }

                return current.result &&
                       self(self, *current.result);
            };

            return visit(visit, type);
        };

    for (auto& parameter : function.parameters) {
        parameter.typeInfo =
            resolveTypeVariables(parameter.typeInfo);

        parameter.type =
            parameter.typeInfo.kind == LanguageType::Kind::Scalar
                ? parameter.typeInfo.scalar
                : ValueType::Error;

        // Generalized variables are valid. Ungeneralized variables
        // indicate that inference did not determine a usable type.
        if (hasUnquantifiedTypeVariable(parameter.typeInfo)) {
            error(
                function.location,
                "TypeInferenceError",
                "Could not infer the type of parameter '" +
                parameter.name + "' in function '" +
                function.name + "'"
            );
        }
    }

    function.returnTypeInfo =
        resolveTypeVariables(function.returnTypeInfo);

    function.returnType =
        function.returnTypeInfo.kind == LanguageType::Kind::Scalar
            ? function.returnTypeInfo.scalar
            : ValueType::Error;

    if (hasUnquantifiedTypeVariable(function.returnTypeInfo)) {
        error(
            function.location,
            "TypeInferenceError",
            "Could not infer the return type of '" +
            function.name +
            "'. Add an explicit return type annotation"
        );
    }

    resolveIRBlockTypes(function.body);
}

} // namespace cmath::language::detail
