#include "LanguageInternal.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <numbers>
#include <sstream>
#include <type_traits>
#include <utility>
#include <stdexcept>

namespace cmath::language::detail {
    namespace {
        struct RuntimeFault : std::runtime_error {
            RuntimeFault(SourceLocation where, std::string code, std::string message) 
            : std::runtime_error(std::move(message)), location(where), diagnosticCode(std::move(code)) {}
            SourceLocation location;
            std::string diagnosticCode;
        };

        bool hasUnresolvedTypeVariable(const LanguageType& type) {
            if (type.kind == LanguageType::Kind::TypeVariable) {
                return true;
            }

            if (type.kind == LanguageType::Kind::Function) {
                for (const auto& parameter : type.parameters) {
                    if (hasUnresolvedTypeVariable(parameter)) return true;
                }

                return type.result && hasUnresolvedTypeVariable(*type.result);
            }

            return false;
        }

        bool runtimePayloadMatches(const RuntimeValue& value) {
            if (value.typeInfo.isFunction()) {
                return std::holds_alternative<std::shared_ptr<RuntimeCallable>>(
                    value.data
                );
            }

            switch (value.type) {
                case ValueType::Integer:
                    return std::holds_alternative<std::int64_t>(value.data);
                case ValueType::Rational:
                    return std::holds_alternative<RationalValue>(value.data);
                case ValueType::Real:
                    return std::holds_alternative<double>(value.data);
                case ValueType::Complex:
                    return std::holds_alternative<std::complex<double>>(value.data);
                case ValueType::Boolean:
                    return std::holds_alternative<bool>(value.data);
                case ValueType::String:
                    return std::holds_alternative<std::string>(value.data);
                case ValueType::Unit:
                    return std::holds_alternative<std::monostate>(value.data);
                case ValueType::Error:
                    return true;
            }

            return false;
        }

        bool checkedAdd(std::int64_t a, std::int64_t b, std::int64_t& out) {
            if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) || 
                (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) return false;
            out = a+b;
            return true;
        }

        bool checkedSub(std::int64_t a, std::int64_t b, std::int64_t& out) {
            if ((b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) || 
                (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b)) return false;
            out = a-b;
            return true;
        }

        bool checkedMul(std::int64_t a, std::int64_t b, std::int64_t& out) {
            if (a == 0 || b == 0) {out = 0; return true;}
            if ((a == -1 && b == std::numeric_limits<std::int64_t>::min()) ||
                (b == -1 && a == std::numeric_limits<std::int64_t>::min())) return false;
            if (a > 0) {
                if (b > 0) {if (a > std::numeric_limits<std::int64_t>::max() / b) return false;}
                else {if (b < std::numeric_limits<std::int64_t>::min()/a) return false;}
            } else {
                if (b > 0) {if (a < std::numeric_limits<std::int64_t>::min() / b) return false;}
                else {if (a < std::numeric_limits<std::int64_t>::max() / b) return false;}
            }
            out = a*b;
            return true;
        }

        std::int64_t gcdAbs(std::int64_t a, std::int64_t b) {
            auto magnitude = [](std::int64_t value) -> std::uint64_t {
                return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1
                                 : static_cast<std::uint64_t>(value);
            };
            const auto g = std::gcd(magnitude(a), magnitude(b));
            if (g > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return std::numeric_limits<std::int64_t>::max();
            return static_cast<std::int64_t>(g);
        }

        RationalValue normalizeRational(std::int64_t numerator, std::int64_t denominator,
                                        const SourceLocation& loc) {
            if (denominator == 0) throw RuntimeFault(loc, "MathError", "Division by zero");
            if (denominator < 0) {
                if (denominator == std::numeric_limits<std::int64_t>::min() ||
                    numerator == std::numeric_limits<std::int64_t>::min())
                    throw RuntimeFault(loc, "MemoryError", "Rational value exceeds supported 64-bit range");
                denominator = -denominator;
                numerator = -numerator;
            }
            const auto divisor = gcdAbs(numerator, denominator);
            if (divisor > 1) {numerator /= divisor; denominator /= divisor;}
            return {numerator, denominator};
        }

        RationalValue rationalAdd(const RationalValue& a, const RationalValue& b, const SourceLocation& loc) {
            const auto g = std::gcd(a.denominator, b.denominator);
            const auto leftFactor = b.denominator / g;
            const auto rightFactor = a.denominator/g;
            std::int64_t left = 0, right = 0, numerator = 0, denominator = 0;
            if (!checkedMul(a.numerator, leftFactor, left) || 
                !checkedMul(b.numerator, rightFactor, right) ||
                !checkedAdd(left, right, numerator) ||
                !checkedMul(a.denominator, leftFactor, denominator))
                throw RuntimeFault(loc, "MemoryError", "Rational arithmetic overflows supported 64-bit range");
            return normalizeRational(numerator, denominator, loc);
        }

        RationalValue rationalSub(const RationalValue& a, const RationalValue& b, const SourceLocation& loc) {
            if (b.numerator == std::numeric_limits<std::int64_t>::min()) 
                throw RuntimeFault(loc, "MemoryError", "Rational arithmetic overflows supported 64-bit range");
            return rationalAdd(a, RationalValue{-b.numerator, b.denominator}, loc);
        }

        RationalValue rationalMul(const RationalValue& a, const RationalValue& b,
                                  const SourceLocation& loc) {
            const auto g1 = gcdAbs(a.numerator, b.denominator);
            const auto g2 = gcdAbs(b.numerator, a.denominator);
            const auto an = a.numerator / (g1 == 0 ? 1 : g1);
            const auto bd = b.denominator / (g1 == 0 ? 1 : g1);
            const auto bn = b.numerator / (g2 == 0 ? 1 : g2);
            const auto ad = a.denominator / (g2 == 0 ? 1 : g2);
            std::int64_t n = 0, d = 0;
            if (!checkedMul(an, bn, n) || !checkedMul(ad, bd, d))
                throw RuntimeFault(loc, "MemoryError", "Rational arithmetic overflows supported 64-bit range");
            return normalizeRational(n, d, loc);
        }

        RationalValue rationalDiv(const RationalValue& a, const RationalValue& b, const SourceLocation& loc) {
            if (b.numerator == 0) throw RuntimeFault(loc, "MathError", "Division by zero");
            return rationalMul(a, normalizeRational(b.denominator, b.numerator, loc), loc);
        }

        RationalValue parseDecimal(const std::string& text, const SourceLocation& loc) {
            const auto dot = text.find('.');
            if (dot == std::string::npos) {
                std::size_t consumed = 0;
                const auto n = std::stoll(text, &consumed);
                if (consumed != text.size()) throw RuntimeFault(loc, "ParseError", "Invalid numeric literal");
                return {n, 1};
            }
            const std::string whole = text.substr(0, dot);
            const std::string fractional = text.substr(dot+1);
            std::int64_t denominator = 1;
            for (std::size_t i = 0; i<fractional.size(); ++i) {
                if (!checkedMul(denominator, 10, denominator))
                    throw RuntimeFault(loc, "MemoryError", "Decimal literal exceeds supported exact rational range");
            }
            std::int64_t wholeValue = 0;
            if (!whole.empty()) wholeValue = std::stoll(whole);
            const std::int64_t fractionalValue = fractional.empty() ? 0 : std::stoll(fractional);
            std::int64_t scaledWhole = 0, numerator = 0;
            if (!checkedMul(wholeValue, denominator, scaledWhole) ||
                !checkedAdd(scaledWhole, fractionalValue, numerator))
                throw RuntimeFault(loc, "MemoryError", "Decimal literal exceeds the supported exact rational range");
            return normalizeRational(numerator, denominator, loc);
        }

        std::string decodeString(const std::string& raw) {
            if (raw.size() < 2) return {};
            std::string result;
            for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
                if (raw[i] != '\\') {result.push_back(raw[i]); continue;}
                if (++i + 1 > raw.size()) break;
                switch (raw[i]) {
                    case 'n': result.push_back('\n'); break;
                    case 't': result.push_back('\t'); break;
                    case 'r': result.push_back('\r'); break;
                    case '\\': result.push_back('\\'); break;
                    case '\'': result.push_back('\''); break;
                    case '"': result.push_back('"'); break;
                    default: result.push_back(raw[i]); break;
                }
            }
            return result;
        }

        long double numericAsLongDouble(const RuntimeValue& value) {
            switch (value.type) {
                case ValueType::Integer: return static_cast<long double>(std::get<std::int64_t>(value.data));
                case ValueType::Rational: {
                    const auto r = std::get<RationalValue>(value.data);
                    return static_cast<long double>(r.numerator) / static_cast<long double>(r.denominator);
                }
                case ValueType::Real: return static_cast<long double>(std::get<double>(value.data));
                case ValueType::Complex: return static_cast<long double>(std::get<std::complex<double>>(value.data).real());
                default: return 0;
            }
        }
    }

    Interpreter::Interpreter(const ExecutionOptions& options) : options_(options) {}

    void Interpreter::runtimeError(const SourceLocation& location, std::string code, std::string message) {
        diagnostics_.push_back(Diagnostic{DiagnosticSeverity::Error, std::move(code), std::move(message), location, {}});
    }

    bool Interpreter::consumeInstruction(const SourceLocation& location) {
        ++instructionCount_;
        if (instructionCount_ > options_.maximumInstructions) {
            runtimeError(location, "RuntimeError", "Execution stopped because instruction budget was exceeded");
            return false;
        }
        return true;
    }

    RuntimeValue Interpreter::promote(
    const RuntimeValue& value,
    ValueType target,
    const SourceLocation& location
    ) {
        // Validate the payload BEFORE the same-type early return.
        bool payloadMatches = false;

        switch (value.type) {
            case ValueType::Integer:
                payloadMatches = std::holds_alternative<std::int64_t>(value.data);
                break;
            case ValueType::Rational:
                payloadMatches = std::holds_alternative<RationalValue>(value.data);
                break;
            case ValueType::Real:
                payloadMatches = std::holds_alternative<double>(value.data);
                break;
            case ValueType::Complex:
                payloadMatches = std::holds_alternative<std::complex<double>>(value.data);
                break;
            case ValueType::Boolean:
                payloadMatches = std::holds_alternative<bool>(value.data);
                break;
            case ValueType::String:
                payloadMatches = std::holds_alternative<std::string>(value.data);
                break;
            case ValueType::Unit:
                payloadMatches = std::holds_alternative<std::monostate>(value.data);
                break;
            case ValueType::Error:
                payloadMatches = true;
                break;
        }

        if (!payloadMatches) {
            throw RuntimeFault(
                location,
                "InternalTypeError",
                "Runtime payload mismatch: type=" +
                typeName(value.type) +
                ", variant index=" +
                std::to_string(value.data.index())
            );
        }

        if (value.type == target) {
            return value;
        }

        if (!isNumeric(value.type) || !canWiden(value.type, target)) {
            throw RuntimeFault(
                location,
                "RuntimeError",
                "Invalid runtime conversion from " +
                typeName(value.typeInfo) + " to " +
                typeName(target)
            );
        }

        if (target == ValueType::Rational) {
            if (value.type == ValueType::Integer) {
                return {
                    target,
                    RationalValue{std::get<std::int64_t>(value.data), 1}
                };
            }
        }

        if (target == ValueType::Real) {
            if (value.type == ValueType::Integer) {
                return {
                    target,
                    static_cast<double>(std::get<std::int64_t>(value.data))
                };
            }

            if (value.type == ValueType::Rational) {
                const auto r = std::get<RationalValue>(value.data);

                return {
                    target,
                    static_cast<double>(r.numerator) /
                    static_cast<double>(r.denominator)
                };
            }
        }

        if (target == ValueType::Complex) {
            if (value.type == ValueType::Integer) {
                return {
                    target,
                    std::complex<double>(
                        static_cast<double>(std::get<std::int64_t>(value.data)),
                        0
                    )
                };
            }

            if (value.type == ValueType::Rational) {
                const auto r = std::get<RationalValue>(value.data);

                return {
                    target,
                    std::complex<double>(
                        static_cast<double>(r.numerator) /
                        static_cast<double>(r.denominator),
                        0
                    )
                };
            }

            if (value.type == ValueType::Real) {
                return {
                    target,
                    std::complex<double>(
                        std::get<double>(value.data),
                        0
                    )
                };
            }
        }

        throw RuntimeFault(
            location,
            "RuntimeError",
            "Unsupported runtime conversion"
        );
    }

    std::string Interpreter::format(const RuntimeValue& value) const {
        std::ostringstream stream;
        if (value.typeInfo.isFunction()) {
            return "<function>";
        }
        switch (value.type) {
            case ValueType::Integer: return std::to_string(std::get<std::int64_t>(value.data));
            case ValueType::Rational: {
                const auto r = std::get<RationalValue>(value.data);
                if (r.denominator == 1) return std::to_string(r.numerator);
                return std::to_string(r.numerator) + "/" + std::to_string(r.denominator);
            }
            case ValueType::Real: {
                const auto* real = std::get_if<double>(&value.data);
                if (!real) {
                    throw std::runtime_error("Real payload mismatch in format() : variant index="+std::to_string(value.data.index()) + ", typeInfo=" + typeName(value.typeInfo));
                }
                stream << std::setprecision(15) << *real;
                return stream.str();
            }
            case ValueType::Complex: {
                const auto z = std::get<std::complex<double>>(value.data);
                stream << std::setprecision(15) << z.real();
                if (z.imag() >= 0) stream << "+";
                stream << std::setprecision(15) << z.imag() << "i";
                return stream.str();
            }
            case ValueType::Boolean: return std::get<bool>(value.data) ? "true" : "false";
            case ValueType::String: return std::get<std::string>(value.data);
            case ValueType::Unit: return "unit";
            case ValueType::Error: return "<error>";
        }
        return "<unknown>";
    }

    RuntimeValue Interpreter::evaluate(const IRExprPtr& expression, Environment& environment) {
        if (!expression) {
            runtimeError({}, "RuntimeError", "Attempted to evaluate an empty expression.");
            return {};
        }
        
        if (!consumeInstruction(expression->location)) return {};
        try {
            switch (expression->kind) {
                case IRExpression::Kind::Literal: {
                    if (expression->type == ValueType::Integer) {
                        std::size_t consumed = 0;
                        const auto value = std::stoll(expression->text, &consumed);
                        if (consumed != expression->text.size()) 
                            throw RuntimeFault(expression->location, "RuntimeError", "Invalid integer literal");
                        return {ValueType::Integer, static_cast<std::int64_t>(value)};
                    }
                    
                    if (expression->type == ValueType::Rational) 
                        return {ValueType::Rational, parseDecimal(expression->text, expression->location)};
                    
                    if (expression->type == ValueType::Real) {
                        std::size_t consumed = 0;
                        const double value = std::stod(expression->text, &consumed);
                        if (consumed != expression->text.size() || !std::isfinite(value))
                            throw RuntimeFault(expression->location, "RuntimeError", "Invalid or non-finite real literal");
                        return {ValueType::Real, value};
                    }

                    if (expression->type == ValueType::String) 
                        return {ValueType::String, decodeString(expression->text)};
                    if (expression->type == ValueType::Boolean) 
                        return {ValueType::Boolean, expression->text == "true"};
                    break;
                }

                case IRExpression::Kind::Variable: {
                    if (expression->text == "pi") return {ValueType::Real, std::numbers::pi};
                    if (expression->text == "e") return {ValueType::Real, std::numbers::e};
                    if (program_) {
                        const auto function = program_->functions.find(expression->text);
                        if (function != program_->functions.end()) {
                            auto callable = std::make_shared<RuntimeCallable>();

                            callable->function = std::make_shared<IRFunction>(function->second);
                            callable->closure = globals_;
                            return RuntimeValue(expression->typeInfo, RuntimeData{std::move(callable)});
                        }
                    }

                    for (Environment *current = &environment; current; current = current->parent.get()) {
                        const auto found = current->values.find(expression->text);
                        if (found != current->values.end()) return *found->second;
                    } 
                    throw RuntimeFault(expression->location, "RuntimeError", "Variable '" + expression->text + "' is not available at runtime");
                }

                case IRExpression::Kind::ComplexImaginary:
                    return {ValueType::Complex, std::complex<double>(0, 1)};

                case IRExpression::Kind::Increment: {
                    if (expression->children.empty() ||
                        expression->children.front()->kind != IRExpression::Kind::Variable) {
                        throw RuntimeFault(
                            expression->location,
                            "RuntimeError",
                            "Invalid increment/decrement target."
                        );
                    }

                    const std::string& name = expression->children.front()->text;

                    Environment* target = &environment;

                    while (target && !target->values.contains(name)) {
                        target = target->parent.get();
                    }

                    if (!target) {
                        throw RuntimeFault(
                            expression->location,
                            "RuntimeError",
                            "Increment/decrement target was not found."
                        );
                    }

                    const auto mutableBinding = target->mutableBindings.find(name);

                    if (mutableBinding == target->mutableBindings.end() ||
                        !mutableBinding->second) {
                        throw RuntimeFault(
                            expression->location,
                            "MutabilityError",
                            "Cannot modify immutable variable '" + name + "'."
                        );
                    }

                    const RuntimeValue previous = *target->values.at(name);
                    RuntimeValue updated = previous;

                    const bool increment = expression->text == "++";

                    switch (previous.type) {
                        case ValueType::Integer: {
                            const auto value = std::get<std::int64_t>(previous.data);
                            std::int64_t result = 0;

                            const bool succeeded = increment
                                ? checkedAdd(value, 1, result)
                                : checkedSub(value, 1, result);

                            if (!succeeded) {
                                throw RuntimeFault(
                                    expression->location,
                                    "OverflowError",
                                    "Integer increment/decrement overflow."
                                );
                            }

                            updated.data = result;
                            break;
                        }

                        case ValueType::Rational: {
                            const auto value = std::get<RationalValue>(previous.data);

                            updated.data = increment
                                ? rationalAdd(value, RationalValue{1, 1}, expression->location)
                                : rationalSub(value, RationalValue{1, 1}, expression->location);

                            break;
                        }

                        case ValueType::Real: {
                            const double value = std::get<double>(previous.data);
                            const double result = increment ? value + 1.0 : value - 1.0;

                            if (!std::isfinite(result)) {
                                throw RuntimeFault(
                                    expression->location,
                                    "OverflowError",
                                    "Real increment/decrement produced a non-finite value."
                                );
                            }

                            updated.data = result;
                            break;
                        }

                        case ValueType::Complex: {
                            const auto value = std::get<std::complex<double>>(previous.data);

                            const auto result = increment
                                ? value + std::complex<double>{1.0, 0.0}
                                : value - std::complex<double>{1.0, 0.0};

                            if (!std::isfinite(result.real()) ||
                                !std::isfinite(result.imag())) {
                                throw RuntimeFault(
                                    expression->location,
                                    "OverflowError",
                                    "Complex increment/decrement overflow."
                                );
                            }

                            updated.data = result;
                            break;
                        }

                        default:
                            throw RuntimeFault(
                                expression->location,
                                "TypeError",
                                "Increment/decrement requires a numeric variable."
                            );
                    }

                    *target->values[name] = updated;

                    return expression->postfix ? previous : updated;
                }
                
                case IRExpression::Kind::Unary: {
                    auto value = evaluate(expression->children[0], environment);
                    if (!diagnostics_.empty()) return {};
                    return applyUnary(expression->text, value, expression->location);
                }

                case IRExpression::Kind::Binary: {
                    if (expression->children.size() != 2 || !expression->children[0] || !expression->children[1])
                        throw RuntimeFault(expression->location, "RuntimeError", "Malformed binary expression.");

                
                    const auto& leftExpression = expression->children[0];
                    const auto& rightExpression = expression->children[1];
                    auto left = evaluate(leftExpression, environment);
                    if (!diagnostics_.empty()) return {};
                    if (expression->text == "and" || expression->text == "or") {
                        if (left.type != ValueType::Boolean) 
                            throw RuntimeFault(expression->location, "TypeError", "Logical operators require Boolean operands.");

                        const bool leftValue = std::get<bool>(left.data);
                        if (expression->text == "and" && !leftValue)
                            return {ValueType::Boolean, false};
                        if (expression->text == "or" && leftValue)
                            return {ValueType::Boolean, true};
                            
                        auto right = evaluate(rightExpression, environment);
                        if (!diagnostics_.empty()) return {};
                        if (right.type != ValueType::Boolean)
                            throw RuntimeFault(expression->location, "TypeError", "Logical operators require Boolean operands");
                        return {ValueType::Boolean, std::get<bool>(right.data)};
                    }
                    
                    auto right = evaluate(rightExpression, environment);
                    if (!diagnostics_.empty()) return {};
                    return applyBinary(expression->text, left, right, expression->type, expression->location);
                }

                case IRExpression::Kind::Call: {
                    if (expression->indirectCall) {
                        if (expression->children.empty()) {
                            throw RuntimeFault(expression->location, "RuntimeError", "Indirect function call is missing its callee");
                        }
                        RuntimeValue callee = evaluate(expression->children.front(), environment);

                        if (!diagnostics_.empty()) return {};

                        if (!callee.typeInfo.isFunction()) {
                            throw RuntimeFault(expression->location, "TypeError", "Attempted to call a non-function value");
                        }

                        const auto* callable = std::get_if<std::shared_ptr<RuntimeCallable>>(&callee.data);
                        if (!callable || !*callable) {
                            throw RuntimeFault(expression->location, "RuntimeError", "Function value does not contain a callable");
                        }

                        std::vector<RuntimeValue> arguments;
                        arguments.reserve(expression->children.size() - 1);

                        for (std::size_t i = 1; i < expression->children.size(); ++i) {
                            arguments.push_back(evaluate(expression->children[i], environment));
                            if (!diagnostics_.empty()) return {};
                        }

                        return callCallable(*callable, arguments, expression->location);
                    }
                    std::vector<RuntimeValue> arguments;
                    arguments.reserve(expression->children.size());
                    for (const auto& child: expression->children) {
                        arguments.push_back(evaluate(child, environment));
                        if (!diagnostics_.empty()) return {};
                    }
                    static const std::unordered_map<std::string, bool> builtins {
                        {"print", true}, {"sqrt", true}, {"sin", true}, {"cos", true},
                        {"tan", true}, {"exp", true}, {"log", true}, {"abs", true},
                        {"floor", true}, {"approximate", true}, {"to_integer_exact", true}
                    };
                    if (builtins.contains(expression->text))
                        return callBuiltin(expression->text, arguments, expression->location);
                    return callFunction(expression->text, arguments, expression->location);
                }
            }
        } catch (const RuntimeFault& fault) {
            runtimeError(fault.location, fault.diagnosticCode, fault.what());
        } catch (const std::exception& exception) {
            runtimeError(expression->location, "RuntimeError", exception.what());
        }
        return {};
    }

    RuntimeValue Interpreter::applyUnary(const std::string& op, const RuntimeValue& value, const SourceLocation& location) {
        if (op == "not") {
            if (value.type != ValueType::Boolean)
                throw RuntimeFault(location, "TypeError", "'not' requires a Boolean operand");
            return {ValueType::Boolean, !std::get<bool>(value.data)};
        }
        if (op == "+") return value;
        if (op == "-") {
            if (value.type == ValueType::Integer) {
                const auto n = std::get<std::int64_t>(value.data);
                if (n == std::numeric_limits<std::int64_t>::min())
                    throw RuntimeFault(location, "MemoryError", "Integer negation overflow");
                return {ValueType::Integer, -n};
            }
            if (value.type == ValueType::Rational) {
                auto r = std::get<RationalValue>(value.data);
                if (r.denominator == std::numeric_limits<std::int64_t>::min())
                    throw RuntimeFault(location, "MemoryError", "Rational negation overflow");
                r.numerator = -r.numerator;
                return {ValueType::Rational, r};
            }
            if (value.type == ValueType::Real) return {ValueType::Real, -std::get<double>(value.data)};
            if (value.type == ValueType::Complex) return {ValueType::Complex, -std::get<std::complex<double>>(value.data)};
        }
        throw RuntimeFault(location, "RuntimeError", "Unsupported unary operation");
    }

    RuntimeValue Interpreter::applyBinary(const std::string& op, const RuntimeValue& leftValue, const RuntimeValue& rightValue, ValueType resultType, const SourceLocation& location) {
        if (op == "+" && leftValue.type == ValueType::String && rightValue.type == ValueType::String) 
            return {ValueType::String, std::get<std::string>(leftValue.data) + std::get<std::string>(rightValue.data)};
        
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            bool result = false;
            if (isNumeric(leftValue.type) && isNumeric(rightValue.type)) {
                const auto common = leftValue.type == ValueType::Complex || rightValue.type == ValueType::Complex
                    ? ValueType::Complex : (leftValue.type == ValueType::Real || rightValue.type == ValueType::Real
                        ? ValueType::Real : ValueType::Rational);
                if (common == ValueType::Complex) {
                    const auto a = std::get<std::complex<double>>(promote(leftValue, common, location).data);
                    const auto b = std::get<std::complex<double>>(promote(rightValue, common, location).data);
                    if (op != "==" && op != "!=") throw RuntimeFault(location, "DomainError", "Complex numebers cannot be ordered");
                    result = op == "==" ? (a == b) : (a != b);
                } else {
                    const long double a = numericAsLongDouble(leftValue);
                    const long double b = numericAsLongDouble(rightValue);
                    if (op == "==") result = a == b;
                    else if (op == "!=") result = a != b;
                    else if (op == "<") result = a < b;
                    else if (op == ">") result = a > b;
                    else if (op == "<=") result = a <= b;
                    else result = a >= b;
                }
            } else if (leftValue.type == ValueType::String && rightValue.type == ValueType::String) {
                const auto& a = std::get<std::string>(leftValue.data);
                const auto& b = std::get<std::string>(rightValue.data);
                if (op == "==") result = a == b;
                else if (op == "!=") result = a != b;
                else if (op == "<") result = a < b;
                else if (op == ">") result = a > b;
                else if (op == "<=") result = a <= b;
                else result = a >= b;
            } else if (leftValue.type == ValueType::Boolean && rightValue.type == ValueType::Boolean) {
                const bool a = std::get<bool>(leftValue.data), b = std::get<bool>(rightValue.data);
                result = op == "==" ? a == b : a != b;
            }
            return {ValueType::Boolean, result};
        }

        if (resultType == ValueType::Integer) {
            const auto a = std::get<std::int64_t>(leftValue.data);
            const auto b = std::get<std::int64_t>(rightValue.data);
            std::int64_t result = 0;
            if (op == "^") {
                if (b < 0) throw RuntimeFault(location, "DomainError", "Negative Integer exponents require a Rational result");
                std::uint64_t exponent = static_cast<std::uint64_t>(b);
                std::int64_t base = a;
                std::int64_t powerResult = 1;
                while (exponent) {
                    if (exponent & 1U) {
                        if (!checkedMul(powerResult, base, powerResult))
                            throw RuntimeFault(location, "MemoryError", "Integer exponentiation overflowed supported 64-bit range");
                    }
                    exponent >>= 1U;
                    if (exponent && !checkedMul(base, base, base))
                        throw RuntimeFault(location, "MemoryError", "Integer exponentiation overflowed supported 64-bit range");
                }
                return {ValueType::Integer, powerResult};
            }

            if (op == "+" && checkedAdd(a, b, result)) return {ValueType::Integer, result};
            if (op == "-" && checkedSub(a, b, result)) return {ValueType::Integer, result};
            if (op == "*" && checkedMul(a, b, result)) return {ValueType::Integer, result};
            if (op == "%") {
                if (b == 0) throw RuntimeFault(location, "RuntimeError", "Modulo by zero");
                if (a == std::numeric_limits<std::int64_t>::min() && b == -1)
                    return {ValueType::Integer, a%b};
            }
            throw RuntimeFault(location, "MemoryError", "Integer arithmetic overflows supported 64-bit range");
        }

        if (op == "^" && (rightValue.type == ValueType::Complex || (leftValue.type == ValueType::Complex && rightValue.type != ValueType::Integer))) {
            const auto base = std::get<std::complex<double>>(promote(leftValue, ValueType::Complex, location).data);
            const auto exponent = std::get<std::complex<double>>(promote(rightValue, ValueType::Complex, location).data);

            if (!std::isfinite(base.real()) || !std::isfinite(base.imag()) || !std::isfinite(exponent.real()) || !std::isfinite(exponent.imag())) {
                throw RuntimeFault(location, "DomainError", "Complex exponentiation requires finite operands");
            }

            if (base == std::complex<double>(0.0, 0.0)) {
                if (exponent == std::complex<double>(0.0, 0.0)) {
                    return {ValueType::Complex, std::complex<double>(1.0, 0.0)};
                } 

                if (exponent.imag() != 0.0 || exponent.real() < 0.0) {
                    throw RuntimeFault(location, "DomainError", "Zero cannot be raised to a negative or non-real power");
                }

                return {ValueType::Complex, std::complex<double>(0.0, 0.0)};
            }

            const auto result = std::pow(base, exponent);

            if (std::isnan(result.real()) || std::isnan(result.imag())) {
                throw RuntimeFault(location, "DomainError", "Complex exponentiation produced an undefined result");
            }

            if (!std::isfinite(result.real()) || !std::isfinite(result.imag())) {
                throw RuntimeFault(location, "OverflowError", "Complex exponentiation overflowed");
            }

            return {ValueType::Complex, result};
        }

        if (op == "^" && rightValue.type == ValueType::Integer) {
            const auto exponentValue = std::get<std::int64_t>(rightValue.data);
            if (resultType == ValueType::Rational) {
                RationalValue base;
                if (leftValue.type == ValueType::Integer) {
                    base = {std::get<std::int64_t>(leftValue.data), 1};
                }
                else base = std::get<RationalValue>(leftValue.data);
                if (base.numerator == 0 && exponentValue < 0) 
                    throw RuntimeFault(location, "RuntimeError", "Zero cannot be raised to a negative power");
                std::uint64_t exponent = exponentValue < 0
                    ? static_cast<std::uint64_t>(-(exponentValue + 1)) + 1
                    : static_cast<std::uint64_t>(exponentValue);
                RationalValue result{1, 1};
                while (exponent) {
                    if (exponent & 1U) result = rationalMul(result, base, location);
                    exponent >>= 1U;
                    if (exponent) base = rationalMul(base, base, location);
                }
                if (exponentValue < 0) result = normalizeRational(result.denominator, result.numerator, location);
                return {ValueType::Rational, result};
            }

            if (resultType == ValueType::Real) {
                const auto base = std::get<double>(promote(leftValue, ValueType::Real, location).data);
                const double result = std::pow(base, static_cast<double>(exponentValue));
                if (!std::isfinite(result)) throw RuntimeFault(location, "RuntimeError", "Real exponentiation is outside real domain or overflowed");
                return {ValueType::Real, result};
            }

            if (resultType == ValueType::Complex) {
                auto base = std::get<std::complex<double>>(promote(leftValue, ValueType::Complex, location).data);
                if (base == std::complex<double>(0, 0) && exponentValue < 0)
                    throw RuntimeFault(location, "RuntimeError", "Zero cannot be raised to a negative power");
                std::uint64_t exponent = exponentValue < 0
                    ? static_cast<std::uint64_t>(-(exponentValue + 1)) + 1
                    : static_cast<std::uint64_t>(exponentValue);
                std::complex<double> result{1, 0};
                while (exponent) {
                    if (exponent & 1U) result *= base;
                    exponent >>= 1U;
                    if (exponent) base *= base;
                }
                if (exponentValue < 0) result = std::complex<double>(1, 0) / result;
                return {ValueType::Complex, result};
            }
        }

        if (op == "^" && rightValue.type == ValueType::Rational) {
            if (leftValue.type == ValueType::Complex) {
                throw RuntimeFault(location, "FeatureError", "Non-integer powers of Complex bases are not yet supported");
            }
            const auto exponent = std::get<RationalValue>(rightValue.data);
            const double base = static_cast<double>(numericAsLongDouble(leftValue));
            const double power = static_cast<double>(exponent.numerator) / static_cast<double>(exponent.denominator);

            if (base == 0.0 && exponent.numerator < 0) {
                throw RuntimeFault(location, "DomainError", "Zero cannot be raised to a negative power");
            }

            double result = 0.0;
            if (base < 0.0) {
                if ((exponent.denominator % 2) == 0) {
                    throw RuntimeFault(location, "DomainError", "A negative base with an even-denominator Rational exponent has no Real result");
                }
            }
            result = std::pow(-base, power);
            if ((exponent.numerator % 2) != 0) {
                result = -result;
            } else {
                result = std::pow(base, power);
            }

            if (std::isnan(result)) {
                throw RuntimeFault(location, "DomainError", "Rational exponentiation is outside the Real domain");
            }

            if (!std::isfinite(result)) {
                throw RuntimeFault(location, "OverflowError", "Rational exponentiation provided a non-finite result");
            }
            return {ValueType::Real, result};
        }

        if (op == "^" && rightValue.type == ValueType::Real) {
            if (leftValue.type == ValueType::Complex) {
                throw RuntimeFault(location, "FeatureError", "Non-integer powers of Complex bases are not yet supported");
            }

            const double base = static_cast<double>(numericAsLongDouble(leftValue));
            const double exponent = std::get<double>(rightValue.data);

            if (base == 0.0 && exponent < 0.0) {
                throw RuntimeFault(location, "DomainError", "Zero cannot be raised to a negative power");
            }

            const double result = std::pow(base, exponent);

            if (std::isnan(result)) {
                throw RuntimeFault(location, "DomainError", "Real exponentiation is outside the Real domain");
            }

            if (!std::isfinite(result)) {
                throw RuntimeFault(location, "OverflowError", "Real exponentatiation produced a non-finite result");
            }

            return {ValueType::Real, result};
        }

        if (op == "^" && rightValue.type == ValueType::Complex) {
            throw RuntimeFault(location, "FeatureError", "Complex exponents are not yet supported");
        }

        if (resultType == ValueType::Rational) {
            const auto a = promote(leftValue, ValueType::Rational, location);
            const auto b = promote(rightValue, ValueType::Rational, location);
            const auto ar = std::get<RationalValue>(a.data);
            const auto br = std::get<RationalValue>(b.data);
            if (op == "+") return {ValueType::Rational, rationalAdd(ar, br, location)};
            if (op == "-") return {ValueType::Rational, rationalSub(ar, br, location)};
            if (op == "*") return {ValueType::Rational, rationalMul(ar, br, location)};
            if (op == "/") return {ValueType::Rational, rationalDiv(ar, br, location)};
        }

        if (resultType == ValueType::Real) {
            const RuntimeValue promotedLeft =
                promote(leftValue, ValueType::Real, location);

            const RuntimeValue promotedRight =
                promote(rightValue, ValueType::Real, location);

            const auto* leftReal =
                std::get_if<double>(&promotedLeft.data);

            const auto* rightReal =
                std::get_if<double>(&promotedRight.data);

            if (!leftReal || !rightReal) {
                throw RuntimeFault(
                    location,
                    "InternalTypeError",
                    "Real arithmetic received invalid promoted values. "
                    "Left: type=" + typeName(promotedLeft.type) +
                    ", variant=" + std::to_string(promotedLeft.data.index()) +
                    "; Right: type=" + typeName(promotedRight.type) +
                    ", variant=" + std::to_string(promotedRight.data.index())
                );
            }

            const double a = *leftReal;
            const double b = *rightReal;

            if (op == "/" && b == 0.0) {
                throw RuntimeFault(
                    location,
                    "RuntimeError",
                    "Division by zero"
                );
            }

            double result = 0.0;

            if (op == "+") result = a + b;
            else if (op == "-") result = a - b;
            else if (op == "*") result = a * b;
            else if (op == "/") result = a / b;
            else {
                throw RuntimeFault(
                    location,
                    "RuntimeError",
                    "Unsupported real operation"
                );
            }

            if (!std::isfinite(result)) {
                throw RuntimeFault(
                    location,
                    "RuntimeError",
                    "Real arithmetic produced a non-finite value"
                );
            }

            return {ValueType::Real, result};
        }

        if (resultType == ValueType::Complex) {
            const auto a = std::get<std::complex<double>>(promote(leftValue, ValueType::Complex, location).data);
            const auto b = std::get<std::complex<double>>(promote(rightValue, ValueType::Complex, location).data);
            if (op == "/" && b == std::complex<double>(0, 0))
                throw RuntimeFault(location, "RuntimeError", "Division by zero");
            if (op == "+") return {ValueType::Complex, a+b};
            if (op == "-") return {ValueType::Complex, a-b};
            if (op == "*") return {ValueType::Complex, a*b};
            if (op == "/") return {ValueType::Complex, a/b};
        }
        throw RuntimeFault(location, "RuntimeError", "Unsupported binary operation");
    }

    RuntimeValue Interpreter::callBuiltin(const std::string& name, const std::vector<RuntimeValue>& args, const SourceLocation& location) {
        if (name == "print") {
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i) output_ += " ";
                output_ += format(args[i]);
            }
            output_ += "\n";
            return {ValueType::Unit, std::monostate{}};
        }

        if (args.size() != 1) throw RuntimeFault(location, "RuntimeError", "Built-in functions received an invalid argument count");
        const auto& value = args[0];
        if (name == "to_integer_exact") {
            if (value.type == ValueType::Integer) return value;
            long double number = numericAsLongDouble(value);
            if (value.type == ValueType::Complex) {
                const auto z = std::get<std::complex<double>>(value.data);
                if (z.imag() != 0.0) throw RuntimeFault(location, "DomainError", "Complex value with nonzero imaginary component cannot be converted to Integer");
                number = z.real();
            }

            if (!std::isfinite(static_cast<double>(number)) || std::floor(number) != number ||
                number < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
                number > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
                throw RuntimeFault(location, "DomainError", "Value is not representable as an Integer");
            return {ValueType::Integer, static_cast<std::int64_t>(number)};
        }

        if (name == "floor") {
            if (value.type == ValueType::Complex) 
                throw RuntimeFault(location, "DomainError", "floor() is not defined for Complex values");
            const auto number = std::floor(numericAsLongDouble(value));
            if (number < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
                number > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
                throw RuntimeFault(location, "MemoryError", "floor() result exceeds Integer range");
            return {ValueType::Integer, static_cast<std::int64_t>(number)};
        }

        if (name == "approximate") {
            if (value.type == ValueType::Complex) return value;
            return {ValueType::Real, static_cast<double>(numericAsLongDouble(value))};
        }

        if (name == "abs") {
            if (value.type == ValueType::Complex)
                return {ValueType::Real, std::abs(std::get<std::complex<double>>(value.data))};
            
            if (value.type == ValueType::Integer) {
                const auto n = std::get<std::int64_t>(value.data);
                if (n == std::numeric_limits<std::int64_t>::min())
                    throw RuntimeFault(location, "MemoryError", "Absolute value exceeds the Integer range");
                return {ValueType::Integer, n < 0 ? -n : n};
            }

            if (value.type == ValueType::Rational) {
                auto r = std::get<RationalValue>(value.data);
                if (r.numerator == std::numeric_limits<std::int64_t>::min())
                    throw RuntimeFault(location, "MemoryError", "Absolute value exceeds Rational range");
                if (r.numerator < 0) r.numerator = -r.numerator;
                return {ValueType::Rational, r};
            }

            return {ValueType::Real, std::abs(std::get<double>(value.data))};
        }

        const bool complexInput = value.type == ValueType::Complex;
        if (complexInput) {
            const auto z = std::get<std::complex<double>>(value.data);
            if (name == "sqrt") return {ValueType::Complex, std::sqrt(z)};
            if (name == "sin") return {ValueType::Complex, std::sin(z)};
            if (name == "cos") return {ValueType::Complex, std::cos(z)};
            if (name == "tan") return {ValueType::Complex, std::tan(z)};
            if (name == "exp") return {ValueType::Complex, std::exp(z)};
            if (name == "log") {
                if (z == std::complex<double>(0, 0)) throw RuntimeFault(location, "DomainError", "Complex logarithm is undefined at zero");
                return {ValueType::Complex, std::log(z)};
            }
        }

        const double x = static_cast<double>(numericAsLongDouble(value));
        const bool complexContext = program_ && program_->defaultDomain == "Complex";
        if (complexContext) {
            const std::complex<double> z{x, 0.0};
            if (name == "sqrt") return {ValueType::Complex, std::sqrt(z)};
            if (name == "sin") return {ValueType::Complex, std::sin(z)};
            if (name == "cos") return {ValueType::Complex, std::cos(z)};
            if (name == "tan") return {ValueType::Complex, std::tan(z)};
            if (name == "exp") return {ValueType::Complex, std::exp(z)};
            if (name == "log") {
                if (x == 0.0) throw RuntimeFault(location, "DomainError", "Complex logarithm is undefined at zero");
                return {ValueType::Complex, std::log(z)};
            }
        }

        if (name == "sqrt") {
            if (x < 0) throw RuntimeFault(location, "DomainError", "Real square root requires nonnegative argument");
            return {ValueType::Real, std::sqrt(x)};
        }

        if (name == "sin") return {ValueType::Real, std::sin(x)};
        if (name == "cos") return {ValueType::Real, std::cos(x)};
        if (name == "tan") return {ValueType::Real, std::tan(x)};
        if (name == "exp") return {ValueType::Real, std::exp(x)};
        if (name == "log") {
            if (x <= 0) throw RuntimeFault(location, "DomainError", "Real logarithm requires strictly positive argument");
            return {ValueType::Real, std::log(x)};
        }
        throw RuntimeFault(location, "RuntimeError", "Unknown built-in function '" + name + "'");
    }

    std::shared_ptr<Interpreter::Environment> Interpreter::captureEnvironment(const std::shared_ptr<Environment>& environment) {
        if (!environment) return {};
        auto captured = std::make_shared<Environment>();
        captured->values = environment->values;
        captured->mutableBindings = environment->mutableBindings;
        captured->parent = captureEnvironment(environment->parent);

        return captured;
    }

    RuntimeValue Interpreter::callFunction(const std::string& name, const std::vector<RuntimeValue>& arguments, const SourceLocation& location) {
        const auto found = program_->functions.find(name);
        if (found == program_->functions.end())
            throw RuntimeFault(location, "RuntimeError", "Function '" + name + "' was not found");
        return invokeFunction(found->second, arguments, globals_, location);
    }

    RuntimeValue Interpreter::callCallable(const std::shared_ptr<RuntimeCallable>& callable, const std::vector<RuntimeValue>& arguments, const SourceLocation& location) {
        if (!callable || !callable->function || !callable->closure) {
            throw RuntimeFault(location, "RuntimeError", "Attempted to invoke an invalid function value");
        }
        return invokeFunction(*callable->function, arguments, callable->closure, location);
    }

    RuntimeValue Interpreter::invokeFunction(const IRFunction& function, const std::vector<RuntimeValue>& arguments, const std::shared_ptr<Environment>& parent, const SourceLocation& location) {
        if (callDepth_ >= options_.maximumCallDepth) {
            throw RuntimeFault(location, "RuntimeError", "Maximum call depth exceeded");
        }
        if (arguments.size() != function.parameters.size()) 
            throw RuntimeFault(location, "RuntimeError", "Function call argument count mismatch");

        auto local = std::make_shared<Environment>();
        local->parent = parent;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto& parameter = function.parameters[i];
            RuntimeValue value = arguments[i];

            const bool polymorphic = hasUnresolvedTypeVariable(parameter.typeInfo);

            if (parameter.typeInfo.isFunction()) {
                if (!polymorphic && value.typeInfo != parameter.typeInfo) {
                    throw RuntimeFault(location, "TypeError", "Argument '" + parameter.name + "' has type " + typeName(value.typeInfo) + "; expected " + typeName(parameter.typeInfo)); 
                }
            } else if (!polymorphic && parameter.type != ValueType::Error) {
                value = promote(value, parameter.type, location);
            }

            local->values[parameter.name] = std::make_shared<RuntimeValue>(std::move(value));
            local->mutableBindings[parameter.name] = false;
        }

        ++callDepth_;
        const auto execution = executeBlock(function.body, *local);
        --callDepth_;
        if (!diagnostics_.empty()) return {};
        if (execution.flow == ControlFlow::Return) return execution.value;
        if (execution.flow != ControlFlow::Normal)
            throw RuntimeFault(location, "ControlFlowError", "Loop control escaped from function '" + function.name + "'");
        if (function.returnType == ValueType::Unit)
            return {ValueType::Unit, std::monostate{}};
        throw RuntimeFault(location, "RuntimeError", "Function completed without returning a value");
    }
 
    Interpreter::StatementExecutionResult Interpreter::executeBlock(const IRBlock& block, Environment& environment)
    {
        for (const auto& statement : block.statements) {
            auto result = executeStatement(statement, environment);
            if (!diagnostics_.empty()) return {};
            if (result.flow != ControlFlow::Normal) return result;
        }
        return {};
    }

    Interpreter::StatementExecutionResult Interpreter::executeStatement(const IRStatement& statement, Environment& environment) {
        if (!consumeInstruction(statement.location)) return {};

        try {
            return std::visit(
                [this, &statement, &environment](const auto& node) -> StatementExecutionResult {
                    using T = std::decay_t<decltype(node)>;

                    if constexpr(std::is_same_v<T, IRDeclaration>) {
                        RuntimeValue value;
                        const bool polymorphic = hasUnresolvedTypeVariable(node.typeInfo);
                        if (node.lambdaFunction) {
                            auto callable = std::make_shared<RuntimeCallable>();
                            callable->function = node.lambdaFunction;
                            callable->closure = captureEnvironment(environment.shared_from_this());
                            value = RuntimeValue(node.typeInfo, RuntimeData{std::move(callable)});

                        } else {
                            value = evaluate(node.initializer, environment);
                            if (!diagnostics_.empty()) return {};
                        }
                        
                        if (node.typeInfo.isFunction()) {
                            if (value.typeInfo != node.typeInfo) {
                                throw RuntimeFault(statement.location, "TypeError", "Cannot initialize '" + node.name + "' with " + typeName(value.typeInfo) + "; expects " + typeName(node.typeInfo));
                            }
                        } else if (!polymorphic && node.type != ValueType::Error) {
                            value = promote(value, node.type, statement.location);
                        }

                        environment.values[node.name] = std::make_shared<RuntimeValue>(std::move(value));
                        environment.mutableBindings[node.name] = node.mutableBinding;
                        lastValue_ = format(*environment.values[node.name]);
                        return {};
                    } else if constexpr(std::is_same_v<T, IRAssignment>) {
                        auto value = evaluate(node.expression, environment);
                        const bool polymorphic = hasUnresolvedTypeVariable(node.typeInfo);
                        if (!diagnostics_.empty()) return {};
                        if (node.typeInfo.isFunction()) {
                            if (value.typeInfo != node.typeInfo) {
                                throw RuntimeFault(statement.location, "TypeError", "Cannot initialize '" + node.name + "' with " + typeName(value.typeInfo) + "; expects " + typeName(node.typeInfo));
                            }
                        } else if (!polymorphic && node.type != ValueType::Error){
                            value = promote(value, node.type, statement.location);
                        }

                        Environment* target = &environment;
                        while (target && !target->values.contains(node.name)) 
                            target = target->parent.get();
                        
                        if (!target)
                            throw RuntimeFault(statement.location, "RuntimeError", "Assignment target '" + node.name + "' is unavailable");

                        const auto mutableBinding = target->mutableBindings.find(node.name);
                        if (mutableBinding == target->mutableBindings.end() || !mutableBinding->second) 
                            throw RuntimeFault(statement.location, "MutabilityError", "Assignment target '" + node.name + "' is not mutable");

                        *target->values[node.name] = value;
                        lastValue_ = format(value);
                        return {};
                    } else if constexpr(std::is_same_v<T, IRExpressionStatement>) {
                        auto value = evaluate(node.expression, environment);
                        if (!diagnostics_.empty()) return {};

                        if (value.type != ValueType::Unit) 
                            lastValue_ = format(value);
                        
                        return {};
                    
                    } else if constexpr(std::is_same_v<T, IRReturn>) {
                        RuntimeValue value{ValueType::Unit, std::monostate{}};
                        const bool polymorphic = hasUnresolvedTypeVariable(node.typeInfo);
                        if (node.expression) {
                            value = evaluate(node.expression, environment);
                            if (!diagnostics_.empty()) return {};
                            if (node.typeInfo.isFunction()) {
                                if (value.typeInfo != node.typeInfo) {
                                    throw RuntimeFault(statement.location, "TypeError", "Cannot return " + typeName(value.typeInfo) + "; expected " + typeName(node.typeInfo));
                                } 
                            } else if (!polymorphic || node.type != ValueType::Error) {
                                value = promote(value, node.type, statement.location);
                            }
                        }
                        return {ControlFlow::Return, value};
                    } else if constexpr(std::is_same_v<T, IRIf>) {
                        for (const auto& branch : node.branches) {
                            if (!branch.condition || !branch.body) {
                                throw RuntimeFault(statement.location, "RuntimeError", "Malformed conditional branch");
                            }
                            const auto condition = evaluate(branch.condition, environment);
                            if (!diagnostics_.empty()) return {};
                            if (condition.type != ValueType::Boolean) {
                                throw RuntimeFault(statement.location, "TypeError", "Conditional expression did not evaluate to Boolean");
                            }

                            if (std::get<bool>(condition.data)) {
                                auto branchEnvironment = std::make_shared<Environment>();
                                branchEnvironment->parent = environment.shared_from_this();
                                return executeBlock(*branch.body, *branchEnvironment);
                            }
                        }

                        if (node.elseBody) {
                            auto elseEnvironment = std::make_shared<Environment>();
                            elseEnvironment->parent = environment.shared_from_this();
                            return executeBlock(*node.elseBody, *elseEnvironment);
                        }
                        return {};
                    } else if constexpr(std::is_same_v<T, IRWhile>) {
                        if (!node.condition || !node.body) {
                            throw RuntimeFault(statement.location, "RuntimeError", "Malformed while loop");
                        }
                        while (true) {
                            const auto condition = evaluate(node.condition, environment);
                            if (!diagnostics_.empty()) return {};
                            if (condition.type != ValueType::Boolean)
                                throw RuntimeFault(statement.location, "TypeError", "While condition did not evaluate to Boolean");
                            if (!std::get<bool>(condition.data)) break;
                            if (!consumeInstruction(statement.location)) return {};

                            auto iterationEnvironment = std::make_shared<Environment>();
                            iterationEnvironment->parent = environment.shared_from_this();
                            const auto bodyResult = executeBlock(*node.body, *iterationEnvironment);
                            if (!diagnostics_.empty()) return {};
                            if (bodyResult.flow == ControlFlow::Return) return bodyResult;
                            if (bodyResult.flow == ControlFlow::Break) break;
                        }
                        return {};
                    } else if constexpr(std::is_same_v<T, IRFor>) {
                        if (!node.start || !node.end || !node.body) {
                            throw RuntimeFault(statement.location, "RuntimeFault", "Malformed for loop");
                        }

                        const auto startValue = evaluate(node.start, environment);
                        if (!diagnostics_.empty()) return {};
                        const auto endValue = evaluate(node.end, environment);
                        if (!diagnostics_.empty()) return {};
                        if (startValue.type != ValueType::Integer || endValue.type != ValueType::Integer)
                            throw RuntimeFault(statement.location, "TypeError", "For-loop bounds must evaluate to Integer");

                        const auto start = std::get<std::int64_t>(startValue.data);
                        const auto end = std::get<std::int64_t>(endValue.data);

                        auto iteratorEnvironment = std::make_shared<Environment>();
                        iteratorEnvironment->parent = environment.shared_from_this();
                        iteratorEnvironment->mutableBindings[node.iterator] = false;
                        iteratorEnvironment->values[node.iterator] = std::make_shared<RuntimeValue>(ValueType::Integer, static_cast<std::int64_t>(start));

                        for (std::int64_t index = start; index < end;) {
                            if (!consumeInstruction(statement.location)) return {};
                            *iteratorEnvironment->values[node.iterator] = {ValueType::Integer, index};

                            auto iterationEnvironment = std::make_shared<Environment>();
                            iterationEnvironment->parent = iteratorEnvironment;
                            const auto bodyResult = executeBlock(*node.body, *iterationEnvironment);
                            if (!diagnostics_.empty()) return {};
                            if (bodyResult.flow == ControlFlow::Return) return bodyResult;
                            if (bodyResult.flow == ControlFlow::Break) break;
                            ++index;
                        }
                        return {};
                    } else if constexpr(std::is_same_v<T, IRBreak>) {
                        return {ControlFlow::Break, {}};
                    } else if constexpr(std::is_same_v<T, IRContinue>) {
                        return {ControlFlow::Continue, {}};
                    }
                },
                statement.data
            );
        } catch (const RuntimeFault& fault) {
            runtimeError(fault.location, fault.diagnosticCode, fault.what());
        } catch (const std::exception& exception) {
            runtimeError(statement.location, "RuntimeError", exception.what());
        }

        return {};
    }

    RuntimeResult Interpreter::execute(const ProgramIR& program) {
        program_ = &program;
        instructionCount_ = 0;
        callDepth_ = 0;
        globals_ = std::make_shared<Environment>();
        output_.clear();
        lastValue_.reset();
        diagnostics_.clear();

        const auto execution = executeBlock(program.body, *globals_);
        if (diagnostics_.empty() && execution.flow != ControlFlow::Normal) {
            runtimeError({}, "ControlFlowError", "Control flow escaped top-level program.");
        }


        return RuntimeResult{diagnostics_.empty(), std::move(output_), std::move(lastValue_), std::move(diagnostics_)};
    }
}