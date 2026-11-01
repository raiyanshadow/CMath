
#include "cmath/compiler/LLVMBackend.h"

#include "LanguageInternal.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/TargetParser/Host.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cmath {
namespace {

using language::detail::IRExpression;
using language::detail::IRExprPtr;
using language::detail::IRFunction;
using language::detail::IRStatement;
using language::detail::ProgramIR;
using language::detail::ValueType;

class ModuleGenerator {
public:
    ModuleGenerator(llvm::LLVMContext& context, const ProgramIR& program)
        : context_(context),
          module_(std::make_unique<llvm::Module>("cmath.module", context)),
          builder_(context),
          program_(program) {}

    bool generate() {
        declareRuntimeFunctions();

        if (!declareUserFunctions())
            return false;

        for (const auto& [name, function] : program_.functions) {
            if (!emitFunction(function))
                return false;
        }

        if (!emitEntryPoint())
            return false;

        return diagnostics_.empty();
    }

    std::unique_ptr<llvm::Module> takeModule() {
        return std::move(module_);
    }

    const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

private:
    void error(
        const SourceLocation& location,
        std::string code,
        std::string message
    ) {
        diagnostics_.push_back(
            Diagnostic{
                DiagnosticSeverity::Error,
                std::move(code),
                std::move(message),
                location,
                {}
            }
        );
    }

    llvm::Type* llvmType(ValueType type) {
        switch (type) {
            case ValueType::Integer:
                return builder_.getInt64Ty();

            case ValueType::Real:
                return builder_.getDoubleTy();

            case ValueType::Boolean:
                return builder_.getInt1Ty();

            case ValueType::String:
                return builder_.getPtrTy();

            case ValueType::Unit:
                return builder_.getVoidTy();

            case ValueType::Rational:
                error(
                    {},
                    "FeatureError",
                    "Native code generation does not yet support exact Rational storage. A Rational value can be widened to Real"
                );
                return nullptr;

            case ValueType::Complex:
                error(
                    {},
                    "FeatureError",
                    "Native code generation does not yet support Complex values"
                );
                return nullptr;

            case ValueType::Error:
                error(
                    {},
                    "CompileError",
                    "Cannot generate native code for an expression with an invalid type"
                );
                return nullptr;
        }

        error(
            {},
            "CompileError",
            "Unknown CMath value type in native code generation"
        );

        return nullptr;
    }

    llvm::FunctionCallee declareFunction(
        const std::string& name,
        llvm::Type* returnType,
        const std::vector<llvm::Type*>& parameters,
        bool variadic = false
    ) {
        auto* signature = llvm::FunctionType::get(
            returnType,
            parameters,
            variadic
        );

        return module_->getOrInsertFunction(name, signature);
    }

    void declareRuntimeFunctions() {
        printf_ = declareFunction(
            "printf",
            builder_.getInt32Ty(),
            {builder_.getPtrTy()},
            true
        );

        abort_ = declareFunction(
            "abort",
            builder_.getVoidTy(),
            {},
            false
        );

        strcmp_ = declareFunction(
            "strcmp",
            builder_.getInt32Ty(),
            {builder_.getPtrTy(), builder_.getPtrTy()}
        );

        auto* doubleType = builder_.getDoubleTy();

        for (const char* name : {
            "sqrt", "sin", "cos", "tan",
            "exp", "log", "fabs", "floor"
        }) {
            mathFunctions_.emplace(
                name,
                declareFunction(name, doubleType, {doubleType})
            );
        }

        pow_ = declareFunction(
            "pow",
            doubleType,
            {doubleType, doubleType}
        );
    }

    bool declareUserFunctions() {
        for (const auto& [name, function] : program_.functions) {
            std::vector<llvm::Type*> parameters;
            parameters.reserve(function.parameters.size());

            for (const auto& parameter : function.parameters) {
                if (parameter.type == ValueType::Unit) {
                    error(
                        function.location,
                        "NAT036",
                        "Function parameters cannot have type Unit."
                    );
                    return false;
                }

                auto* type = llvmType(parameter.type);

                if (!type)
                    return false;

                parameters.push_back(type);
            }

            auto* returnType = llvmType(function.returnType);

            if (!returnType)
                return false;

            auto* signature = llvm::FunctionType::get(
                returnType,
                parameters,
                false
            );

            auto* nativeFunction = llvm::Function::Create(
                signature,
                llvm::Function::ExternalLinkage,
                functionSymbol(name),
                module_.get()
            );

            userFunctions_.emplace(name, nativeFunction);

            std::size_t index = 0;

            for (auto& argument : nativeFunction->args()) {
                argument.setName(
                    function.parameters[index++].name
                );
            }
        }

        return diagnostics_.empty();
    }

    static std::string functionSymbol(
        const std::string& sourceName
    ) {
        return "cmath_fn_" + sourceName;
    }

    llvm::AllocaInst* createLocal(
        const std::string& name,
        llvm::Type* type
    ) {
        return builder_.CreateAlloca(
            type,
            nullptr,
            name + ".addr"
        );
    }

    llvm::Value* makeString(
        const std::string& value,
        const std::string& name = "str"
    ) {
        return builder_.CreateGlobalString(value, name);
    }

    static std::string decodeString(const std::string& raw) {
        if (raw.size() < 2)
            return {};

        std::string result;

        for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
            if (raw[i] != '\\') {
                result.push_back(raw[i]);
                continue;
            }

            if (i + 1 >= raw.size() - 1)
                break;

            ++i;

            switch (raw[i]) {
                case 'n':
                    result.push_back('\n');
                    break;

                case 't':
                    result.push_back('\t');
                    break;

                case 'r':
                    result.push_back('\r');
                    break;

                case '\\':
                    result.push_back('\\');
                    break;

                case '\'':
                    result.push_back('\'');
                    break;

                case '"':
                    result.push_back('"');
                    break;

                default:
                    result.push_back(raw[i]);
                    break;
            }
        }

        return result;
    }

    llvm::Value* coerce(
        llvm::Value* value,
        ValueType from,
        ValueType to,
        const SourceLocation& location
    ) {
        if (!value)
            return nullptr;

        if (from == to)
            return value;

        if (to == ValueType::Real && from == ValueType::Integer) {
            return builder_.CreateSIToFP(
                value,
                builder_.getDoubleTy(),
                "int.to.real"
            );
        }

        if (to == ValueType::Real && from == ValueType::Rational)
            return value;

        error(
            location,
            "CompileError",
            "Native code generation cannot convert " +
                language::detail::typeName(from) +
                " to " +
                language::detail::typeName(to)
        );

        return nullptr;
    }

    static bool isRealLike(ValueType type) {
        return type == ValueType::Real ||
               type == ValueType::Rational;
    }

    llvm::Value* emitRealExpression(
        const IRExprPtr& expression
    ) {
        return emitExpression(expression, ValueType::Real);
    }

    llvm::Value* emitExpression(
        const IRExprPtr& expression,
        ValueType expected = ValueType::Error
    ) {
        if (!expression)
            return nullptr;

        const bool approximateRational =
            expected == ValueType::Real &&
            expression->type == ValueType::Rational;

        const ValueType effectiveType =
            approximateRational
                ? ValueType::Real
                : (expected == ValueType::Error
                    ? expression->type
                    : expected);

        switch (expression->kind) {
            case IRExpression::Kind::Literal: {
                if (expression->type == ValueType::Integer) {
                    try {
                        const auto value = std::stoll(
                            expression->text
                        );

                        auto* constant = llvm::ConstantInt::getSigned(
                            builder_.getInt64Ty(),
                            value
                        );

                        return coerce(
                            constant,
                            ValueType::Integer,
                            effectiveType,
                            expression->location
                        );
                    } catch (...) {
                        error(
                            expression->location,
                            "CompileError",
                            "Integer literal is outside the supported 64-bit range"
                        );

                        return nullptr;
                    }
                }

                if (
                    expression->type == ValueType::Rational &&
                    expected == ValueType::Real
                ) {
                    try {
                        const auto value = std::stod(
                            expression->text
                        );

                        if (!std::isfinite(value))
                            throw std::out_of_range("non-finite");

                        return llvm::ConstantFP::get(
                            builder_.getDoubleTy(),
                            value
                        );
                    } catch (...) {
                        error(
                            expression->location,
                            "CompileError",
                            "Cannot approximate this Rational literal as a Real value"
                        );

                        return nullptr;
                    }
                }

                if (expression->type == ValueType::Real) {
                    try {
                        const auto value = std::stod(
                            expression->text
                        );

                        if (!std::isfinite(value))
                            throw std::out_of_range("non-finite");

                        return llvm::ConstantFP::get(
                            builder_.getDoubleTy(),
                            value
                        );
                    } catch (...) {
                        error(
                            expression->location,
                            "CompileError",
                            "Invalid or non-finite Real literal"
                        );

                        return nullptr;
                    }
                }

                if (expression->type == ValueType::Boolean) {
                    return llvm::ConstantInt::get(
                        builder_.getInt1Ty(),
                        expression->text == "true"
                    );
                }

                if (expression->type == ValueType::String) {
                    return makeString(
                        decodeString(expression->text),
                        "cmath.string"
                    );
                }

                error(
                    expression->location,
                    "CompileError",
                    "This literal type is not supported by the native backend"
                );

                return nullptr;
            }

            case IRExpression::Kind::Variable: {
                if (expression->text == "pi") {
                    return llvm::ConstantFP::get(
                        builder_.getDoubleTy(),
                        3.14159265358979323846
                    );
                }

                if (expression->text == "e") {
                    return llvm::ConstantFP::get(
                        builder_.getDoubleTy(),
                        2.71828182845904523536
                    );
                }

                const auto found = variables_.find(
                    expression->text
                );

                if (found == variables_.end()) {
                    error(
                        expression->location,
                        "CompileError",
                        "Variable '" + expression->text +
                            "' is unavailable in this native function"
                    );

                    return nullptr;
                }

                auto* loaded = builder_.CreateLoad(
                    found->second.type,
                    found->second.address,
                    expression->text + ".load"
                );

                if (
                    found->second.typeKind == ValueType::Integer &&
                    effectiveType == ValueType::Real
                ) {
                    return coerce(
                        loaded,
                        ValueType::Integer,
                        ValueType::Real,
                        expression->location
                    );
                }

                if (found->second.typeKind != effectiveType) {
                    error(
                        expression->location,
                        "FeatureError",
                        "Native variable type conversion is not supported for '" +
                            expression->text + "'"
                    );

                    return nullptr;
                }

                return loaded;
            }

            case IRExpression::Kind::ComplexImaginary:
                error(
                    expression->location,
                    "FeatureError",
                    "Complex constants are not supported by the initial native backend"
                );

                return nullptr;

            case IRExpression::Kind::Increment: {
                if (expression->children.empty() ||
                    expression->children.front()->kind != IRExpression::Kind::Variable) {
                    error(expression->location, "CompileError", "Invalid increment/decrement target");
                    return nullptr;
                }

                const std::string& name = expression->children.front()->text;
                const auto found = variables_.find(name);

                if (found == variables_.end()) {
                    error(expression->location, "CompileError", "Increment/decrement target '" + name +  "' is unavailable in this native function.");
                    return nullptr;
                }
                
                const bool increment = expression->text == "++";
                auto *previous = builder_.CreateLoad(found->second.type, found->second.address, name  + ".increment.old");

                llvm::Value* updated = nullptr;

                if (found->second.typeKind == ValueType::Integer) {
                    auto* limit = llvm::ConstantInt::getSigned(
                        builder_.getInt64Ty(), increment ? std::numeric_limits<std::int64_t>::max() : std::numeric_limits<std::int64_t>::min()
                    );
                    auto* overflow = builder_.CreateICmpEQ(previous, limit);
                }
            }

            case IRExpression::Kind::Unary: {
                const auto operandType =
                    expression->children.front()->type;

                llvm::Value* operand =
                    effectiveType == ValueType::Real
                        ? emitRealExpression(
                            expression->children.front()
                        )
                        : emitExpression(
                            expression->children.front()
                        );

                if (!operand)
                    return nullptr;

                if (expression->text == "+")
                    return operand;

                if (expression->text == "-") {
                    if (effectiveType == ValueType::Real) {
                        return builder_.CreateFNeg(
                            operand,
                            "real.negate"
                        );
                    }

                    if (operandType == ValueType::Integer) {
                        return builder_.CreateNeg(
                            operand,
                            "integer.negate"
                        );
                    }
                }

                error(
                    expression->location,
                    "CompileError",
                    "Unsupported unary operation in native code generation"
                );

                return nullptr;
            }

            case IRExpression::Kind::Binary:
                return emitBinary(expression, expected);

            case IRExpression::Kind::Call: {
                auto* value = emitCall(expression);

                if (
                    !value ||
                    expected == ValueType::Error ||
                    expression->type == expected
                ) {
                    return value;
                }

                return coerce(
                    value,
                    expression->type,
                    expected,
                    expression->location
                );
            }
        }

        error(
            expression->location,
            "CompileError",
            "Unknown expression kind in native code generation"
        );

        return nullptr;
    }

    llvm::Value* emitBinary(
        const IRExprPtr& expression,
        ValueType expected
    ) {
        const auto& leftExpr = expression->children[0];
        const auto& rightExpr = expression->children[1];
        const std::string& op = expression->text;

        const bool comparison =
            op == "==" || op == "!=" ||
            op == "<" || op == ">" ||
            op == "<=" || op == ">=";

        if (comparison) {
            const auto leftType = leftExpr->type;
            const auto rightType = rightExpr->type;

            if (
                leftType == ValueType::String &&
                rightType == ValueType::String
            ) {
                auto* left = emitExpression(leftExpr);
                auto* right = emitExpression(rightExpr);

                if (!left || !right)
                    return nullptr;

                auto* compared = builder_.CreateCall(
                    strcmp_,
                    {left, right},
                    "string.compare"
                );

                auto* zero = builder_.getInt32(0);

                if (op == "==")
                    return builder_.CreateICmpEQ(compared, zero);

                if (op == "!=")
                    return builder_.CreateICmpNE(compared, zero);

                if (op == "<")
                    return builder_.CreateICmpSLT(compared, zero);

                if (op == ">")
                    return builder_.CreateICmpSGT(compared, zero);

                if (op == "<=")
                    return builder_.CreateICmpSLE(compared, zero);

                return builder_.CreateICmpSGE(compared, zero);
            }

            if (
                leftType == ValueType::Boolean &&
                rightType == ValueType::Boolean
            ) {
                auto* left = emitExpression(leftExpr);
                auto* right = emitExpression(rightExpr);

                if (!left || !right)
                    return nullptr;

                if (op == "==")
                    return builder_.CreateICmpEQ(left, right);

                if (op == "!=")
                    return builder_.CreateICmpNE(left, right);

                error(
                    expression->location,
                    "FeatureError",
                    "Boolean values support equality comparisons only"
                );

                return nullptr;
            }

            if (
                leftType == ValueType::Complex ||
                rightType == ValueType::Complex
            ) {
                error(
                    expression->location,
                    "FeatureError",
                    "Complex comparisons are not yet supported by the native backend"
                );

                return nullptr;
            }

            if (
                (leftType == ValueType::Rational ||
                 rightType == ValueType::Rational) &&
                leftType != ValueType::Real &&
                rightType != ValueType::Real
            ) {
                error(
                    expression->location,
                    "FeatureError",
                    "Comparing Rational values without a Real operand requires exact Rational support, which is not yet available in the native backend"
                );

                return nullptr;
            }

            const bool useReal =
                leftType == ValueType::Real ||
                rightType == ValueType::Real ||
                leftType == ValueType::Rational ||
                rightType == ValueType::Rational;

            llvm::Value* left =
                useReal
                    ? emitRealExpression(leftExpr)
                    : emitExpression(leftExpr);

            llvm::Value* right =
                useReal
                    ? emitRealExpression(rightExpr)
                    : emitExpression(rightExpr);

            if (!left || !right)
                return nullptr;

            if (useReal) {
                if (op == "==")
                    return builder_.CreateFCmpOEQ(left, right);

                if (op == "!=")
                    return builder_.CreateFCmpONE(left, right);

                if (op == "<")
                    return builder_.CreateFCmpOLT(left, right);

                if (op == ">")
                    return builder_.CreateFCmpOGT(left, right);

                if (op == "<=")
                    return builder_.CreateFCmpOLE(left, right);

                return builder_.CreateFCmpOGE(left, right);
            }

            if (op == "==")
                return builder_.CreateICmpEQ(left, right);

            if (op == "!=")
                return builder_.CreateICmpNE(left, right);

            if (op == "<")
                return builder_.CreateICmpSLT(left, right);

            if (op == ">")
                return builder_.CreateICmpSGT(left, right);

            if (op == "<=")
                return builder_.CreateICmpSLE(left, right);

            return builder_.CreateICmpSGE(left, right);
        }

        ValueType effectiveType = expression->type;

        if (
            expected == ValueType::Real &&
            (expression->type == ValueType::Integer ||
             expression->type == ValueType::Rational ||
             expression->type == ValueType::Real)
        ) {
            effectiveType = ValueType::Real;
        }

        if (effectiveType == ValueType::Real) {
            auto* left = emitRealExpression(leftExpr);
            auto* right = emitRealExpression(rightExpr);

            if (!left || !right)
                return nullptr;

            if (op == "+")
                return builder_.CreateFAdd(left, right, "real.add");

            if (op == "-")
                return builder_.CreateFSub(left, right, "real.subtract");

            if (op == "*")
                return builder_.CreateFMul(left, right, "real.multiply");

            if (op == "/") {
                emitZeroCheck(
                    right,
                    true,
                    expression->location,
                    "Division by zero"
                );

                return builder_.CreateFDiv(
                    left,
                    right,
                    "real.divide"
                );
            }

            if (op == "%")
                return builder_.CreateFRem(left, right, "real.remainder");

            if (op == "^") {
                return builder_.CreateCall(
                    pow_,
                    {left, right},
                    "real.power"
                );
            }
        } else if (effectiveType == ValueType::Integer) {
            auto* left = emitExpression(leftExpr);
            auto* right = emitExpression(rightExpr);

            if (!left || !right)
                return nullptr;

            if (op == "+")
                return builder_.CreateAdd(left, right, "integer.add");

            if (op == "-")
                return builder_.CreateSub(left, right, "integer.subtract");

            if (op == "*")
                return builder_.CreateMul(left, right, "integer.multiply");

            if (op == "%") {
                emitZeroCheck(
                    right,
                    false,
                    expression->location,
                    "Remainder by zero"
                );

                return builder_.CreateSRem(
                    left,
                    right,
                    "integer.remainder"
                );
            }

            if (op == "^") {
                error(
                    expression->location,
                    "FeatureError",
                    "Integer exponentiation is not yet supported by the native backend. Convert the base to Real to use pow()"
                );

                return nullptr;
            }
        }

        error(
            expression->location,
            "FeatureError",
            "This arithmetic operation requires a native numeric type that is not yet supported"
        );

        return nullptr;
    }

    void emitZeroCheck(
        llvm::Value* value,
        bool isReal,
        const SourceLocation& location,
        const std::string& message
    ) {
        llvm::Value* isZero =
            isReal
                ? builder_.CreateFCmpOEQ(
                    value,
                    llvm::ConstantFP::get(
                        builder_.getDoubleTy(),
                        0.0
                    )
                )
                : builder_.CreateICmpEQ(
                    value,
                    builder_.getInt64(0)
                );

        emitAbortIf(isZero, location, message);
    }

    void emitAbortIf(
        llvm::Value* condition,
        const SourceLocation& location,
        const std::string& message
    ) {
        auto* function =
            builder_.GetInsertBlock()->getParent();

        auto* failBlock = llvm::BasicBlock::Create(
            context_,
            "cmath.runtime.failure",
            function
        );

        auto* continueBlock = llvm::BasicBlock::Create(
            context_,
            "cmath.runtime.continue",
            function
        );

        builder_.CreateCondBr(
            condition,
            failBlock,
            continueBlock
        );

        builder_.SetInsertPoint(failBlock);

        const auto diagnostic =
            "CMath runtime error at " +
            std::to_string(location.line) + ":" +
            std::to_string(location.column) +
            ": " + message + "\n";

        builder_.CreateCall(
            printf_,
            {makeString(diagnostic, "cmath.runtime.message")}
        );

        builder_.CreateCall(abort_);
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(continueBlock);
    }

    llvm::Value* emitCall(
        const IRExprPtr& expression
    ) {
        const auto& name = expression->text;

        if (name == "print")
            return emitPrint(expression);

        if (const auto user = userFunctions_.find(name);
            user != userFunctions_.end()) {

            const auto sourceFunction =
                program_.functions.find(name);

            std::vector<llvm::Value*> arguments;
            arguments.reserve(expression->children.size());

            for (
                std::size_t i = 0;
                i < expression->children.size();
                ++i
            ) {
                const auto parameterType =
                    sourceFunction->second.parameters[i].type;

                auto* argument = emitExpression(
                    expression->children[i],
                    parameterType
                );

                if (!argument)
                    return nullptr;

                arguments.push_back(argument);
            }

            return builder_.CreateCall(
                user->second,
                arguments,
                sourceFunction->second.returnType == ValueType::Unit
                    ? ""
                    : "call." + name
            );
        }

        if (expression->children.size() != 1) {
            error(
                expression->location,
                "CompileError",
                "Native built-in '" + name +
                    "' has an unsupported argument count"
            );

            return nullptr;
        }

        const auto& argumentExpr =
            expression->children.front();

        if (name == "approximate") {
            if (argumentExpr->type == ValueType::Real)
                return emitExpression(argumentExpr);

            if (argumentExpr->type == ValueType::Integer)
                return emitExpression(argumentExpr, ValueType::Real);

            if (argumentExpr->type == ValueType::Rational)
                return emitRealExpression(argumentExpr);
        }

        if (name == "abs") {
            if (argumentExpr->type == ValueType::Integer) {
                auto* value = emitExpression(argumentExpr);

                if (!value)
                    return nullptr;

                auto* minimum = llvm::ConstantInt::getSigned(
                    builder_.getInt64Ty(),
                    std::numeric_limits<std::int64_t>::min()
                );

                auto* isMinimum = builder_.CreateICmpEQ(
                    value,
                    minimum
                );

                emitAbortIf(
                    isMinimum,
                    expression->location,
                    "Integer absolute value overflow"
                );

                auto* negative = builder_.CreateICmpSLT(
                    value,
                    builder_.getInt64(0)
                );

                auto* negated = builder_.CreateNeg(value);

                return builder_.CreateSelect(
                    negative,
                    negated,
                    value,
                    "integer.abs"
                );
            }

            if (isRealLike(argumentExpr->type)) {
                auto* value = emitRealExpression(argumentExpr);

                if (!value)
                    return nullptr;

                return builder_.CreateCall(
                    mathFunctions_.at("fabs"),
                    {value},
                    "real.abs"
                );
            }
        }

        if (name == "floor") {
            if (argumentExpr->type == ValueType::Integer)
                return emitExpression(argumentExpr);

            if (isRealLike(argumentExpr->type)) {
                auto* value = emitRealExpression(argumentExpr);

                if (!value)
                    return nullptr;

                auto* floored = builder_.CreateCall(
                    mathFunctions_.at("floor"),
                    {value},
                    "real.floor"
                );

                const auto lower = llvm::ConstantFP::get(
                    builder_.getDoubleTy(),
                    -9223372036854775808.0
                );

                const auto upper = llvm::ConstantFP::get(
                    builder_.getDoubleTy(),
                    9223372036854775808.0
                );

                auto* belowRange = builder_.CreateFCmpOLT(
                    floored,
                    lower
                );

                auto* aboveRange = builder_.CreateFCmpOGE(
                    floored,
                    upper
                );

                auto* unordered = builder_.CreateFCmpUNO(
                    floored,
                    floored
                );

                auto* invalid = builder_.CreateOr(
                    belowRange,
                    aboveRange
                );

                invalid = builder_.CreateOr(
                    invalid,
                    unordered
                );

                emitAbortIf(
                    invalid,
                    expression->location,
                    "floor() result is outside the Integer range"
                );

                return builder_.CreateFPToSI(
                    floored,
                    builder_.getInt64Ty(),
                    "floor.to.integer"
                );
            }
        }

        if (name == "to_integer_exact") {
            error(
                expression->location,
                "FeatureError",
                "to_integer_exact() needs a checked native runtime implementation and is not yet supported"
            );

            return nullptr;
        }

        if (
            name == "sqrt" || name == "sin" ||
            name == "cos" || name == "tan" ||
            name == "exp" || name == "log"
        ) {
            if (expression->type != ValueType::Real) {
                error(
                    expression->location,
                    "FeatureError",
                    "Complex-valued transcendental functions are not yet supported by the native backend"
                );

                return nullptr;
            }

            auto* value = emitRealExpression(argumentExpr);

            if (!value)
                return nullptr;

            if (name == "sqrt") {
                auto* negative = builder_.CreateFCmpOLT(
                    value,
                    llvm::ConstantFP::get(
                        builder_.getDoubleTy(),
                        0.0
                    )
                );

                emitAbortIf(
                    negative,
                    expression->location,
                    "Real square root of a negative value"
                );
            } else if (name == "log") {
                auto* nonPositive = builder_.CreateFCmpOLE(
                    value,
                    llvm::ConstantFP::get(
                        builder_.getDoubleTy(),
                        0.0
                    )
                );

                emitAbortIf(
                    nonPositive,
                    expression->location,
                    "Real logarithm of a non-positive value"
                );
            }

            return builder_.CreateCall(
                mathFunctions_.at(name),
                {value},
                name + ".result"
            );
        }

        error(
            expression->location,
            "FeatureError",
            "Built-in '" + name +
                "' is not supported by the initial native backend"
        );

        return nullptr;
    }

    llvm::Value* emitPrint(
        const IRExprPtr& expression
    ) {
        for (
            std::size_t i = 0;
            i < expression->children.size();
            ++i
        ) {
            const auto& argumentExpr =
                expression->children[i];

            if (i != 0) {
                auto* separator = makeString(
                    " ",
                    "cmath.print.separator"
                );

                builder_.CreateCall(
                    printf_,
                    {separator}
                );
            }

            if (argumentExpr->type == ValueType::Integer) {
                auto* value = emitExpression(argumentExpr);

                if (!value)
                    return nullptr;

                builder_.CreateCall(
                    printf_,
                    {
                        makeString("%lld", "cmath.fmt.integer"),
                        value
                    }
                );
            } else if (argumentExpr->type == ValueType::Real) {
                auto* value = emitExpression(argumentExpr);

                if (!value)
                    return nullptr;

                builder_.CreateCall(
                    printf_,
                    {
                        makeString("%.15g", "cmath.fmt.real"),
                        value
                    }
                );
            } else if (argumentExpr->type == ValueType::String) {
                auto* value = emitExpression(argumentExpr);

                if (!value)
                    return nullptr;

                builder_.CreateCall(
                    printf_,
                    {
                        makeString("%s", "cmath.fmt.string"),
                        value
                    }
                );
            } else if (argumentExpr->type == ValueType::Boolean) {
                auto* value = emitExpression(argumentExpr);

                if (!value)
                    return nullptr;

                auto* trueText = makeString(
                    "true",
                    "cmath.bool.true"
                );

                auto* falseText = makeString(
                    "false",
                    "cmath.bool.false"
                );

                auto* text = builder_.CreateSelect(
                    value,
                    trueText,
                    falseText,
                    "boolean.text"
                );

                builder_.CreateCall(
                    printf_,
                    {
                        makeString("%s", "cmath.fmt.boolean"),
                        text
                    }
                );
            } else {
                error(
                    argumentExpr->location,
                    "FeatureError",
                    "print() cannot yet print values of type " +
                        language::detail::typeName(argumentExpr->type) +
                        " in a native executable"
                );

                return nullptr;
            }
        }

        builder_.CreateCall(
            printf_,
            {makeString("\n", "cmath.print.newline")}
        );

        return llvm::ConstantInt::get(
            builder_.getInt32Ty(),
            0
        );
    }

    bool emitStatement(
        const IRStatement& statement
    ) {
        switch (statement.kind) {
            case IRStatement::Kind::Declare: {
                if (statement.type == ValueType::Unit) {
                    error(
                        statement.location,
                        "CompileError",
                        "Unit values cannot be stored in variables"
                    );

                    return false;
                }

                auto* type = llvmType(statement.type);

                if (!type)
                    return false;

                auto* value = emitExpression(
                    statement.expression,
                    statement.type
                );

                if (!value)
                    return false;

                auto* address = createLocal(
                    statement.name,
                    type
                );

                builder_.CreateStore(value, address);

                variables_[statement.name] = Variable{
                    address,
                    type,
                    statement.type
                };

                return true;
            }

            case IRStatement::Kind::Assign: {
                const auto found =
                    variables_.find(statement.name);

                if (found == variables_.end()) {
                    error(
                        statement.location,
                        "CompileError",
                        "Assignment target '" + statement.name +
                            "' is not available in this native function"
                    );

                    return false;
                }

                auto* value = emitExpression(
                    statement.expression,
                    statement.type
                );

                if (!value)
                    return false;

                builder_.CreateStore(
                    value,
                    found->second.address
                );

                return true;
            }

            case IRStatement::Kind::Expression:
                return emitExpression(statement.expression) != nullptr;

            case IRStatement::Kind::Return: {
                if (inMain_) {
                    // Top-level returns exit main with code 0.
                    if (
                        statement.expression &&
                        !emitExpression(statement.expression)
                    ) {
                        return false;
                    }

                    builder_.CreateRet(builder_.getInt32(0));
                    terminated_ = true;

                    return true;
                }

                if (
                    statement.type == ValueType::Unit ||
                    !statement.expression
                ) {
                    builder_.CreateRetVoid();
                    terminated_ = true;

                    return true;
                }

                auto* value = emitExpression(
                    statement.expression,
                    statement.type
                );

                if (!value)
                    return false;

                builder_.CreateRet(value);
                terminated_ = true;

                return true;
            }
        }

        error(
            statement.location,
            "CompileError",
            "Unknown statement kind in native code generation"
        );

        return false;
    }

    bool emitFunction(
        const IRFunction& sourceFunction
    ) {
        const auto functionIt =
            userFunctions_.find(sourceFunction.name);

        if (functionIt == userFunctions_.end())
            return false;

        auto* function = functionIt->second;

        auto* entry = llvm::BasicBlock::Create(
            context_,
            "entry",
            function
        );

        builder_.SetInsertPoint(entry);

        variables_.clear();
        inMain_ = false;
        terminated_ = false;

        std::size_t index = 0;

        for (auto& argument : function->args()) {
            const auto& parameter =
                sourceFunction.parameters[index++];

            auto* type = llvmType(parameter.type);

            if (!type)
                return false;

            auto* address = createLocal(
                parameter.name,
                type
            );

            builder_.CreateStore(&argument, address);

            variables_[parameter.name] = Variable{
                address,
                type,
                parameter.type
            };
        }

        for (const auto& statement : sourceFunction.body) {
            if (terminated_)
                break;

            if (!emitStatement(statement))
                return false;
        }

        if (!terminated_) {
            if (sourceFunction.returnType == ValueType::Unit) {
                builder_.CreateRetVoid();
                terminated_ = true;
            } else {
                error(
                    sourceFunction.location,
                    "CompileError",
                    "Native function '" + sourceFunction.name +
                        "' has no terminating return"
                );

                return false;
            }
        }

        return diagnostics_.empty();
    }

    bool emitEntryPoint() {
        auto* mainType = llvm::FunctionType::get(
            builder_.getInt32Ty(),
            false
        );

        auto* mainFunction = llvm::Function::Create(
            mainType,
            llvm::Function::ExternalLinkage,
            "main",
            module_.get()
        );

        auto* entry = llvm::BasicBlock::Create(
            context_,
            "entry",
            mainFunction
        );

        builder_.SetInsertPoint(entry);

        variables_.clear();
        inMain_ = true;
        terminated_ = false;

        bool emissionSucceeded = true;

        for (
            std::size_t i = 0;
            i < program_.statements.size();
            ++i
        ) {
            if (terminated_)
                break;

            const auto& statement =
                program_.statements[i];

            const auto previousDiagnosticCount =
                diagnostics_.size();

            const bool emitted =
                emitStatement(statement);

            if (!emitted) {
                if (
                    diagnostics_.size() ==
                    previousDiagnosticCount
                ) {
                    error(
                        statement.location,
                        "CompileError",
                        "Failed to generate native code for a top-level statement."
                    );
                }

                emissionSucceeded = false;
                break;
            }
        }

        // Terminate main if it does not contain an explicit return.
        if (!terminated_) {
            builder_.CreateRet(builder_.getInt32(0));
            terminated_ = true;
        }

        inMain_ = false;

        if (!emissionSucceeded || !diagnostics_.empty())
            return false;

        std::string verificationMessage;

        llvm::raw_string_ostream verificationStream(
            verificationMessage
        );

        if (
            llvm::verifyFunction(
                *mainFunction,
                &verificationStream
            )
        ) {
            verificationStream.flush();

            error(
                {},
                "CompileError",
                "Generated main failed verification: " +
                    verificationMessage
            );

            return false;
        }

        return true;
    }

    struct Variable {
        llvm::AllocaInst* address = nullptr;
        llvm::Type* type = nullptr;
        ValueType typeKind = ValueType::Error;
    };

    llvm::LLVMContext& context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    const ProgramIR& program_;

    std::vector<Diagnostic> diagnostics_;

    std::unordered_map<std::string, Variable> variables_;
    std::unordered_map<std::string, llvm::Function*> userFunctions_;

    llvm::FunctionCallee printf_;
    llvm::FunctionCallee abort_;
    llvm::FunctionCallee strcmp_;
    llvm::FunctionCallee pow_;

    std::unordered_map<std::string, llvm::FunctionCallee>
        mathFunctions_;

    bool terminated_ = false;
    bool inMain_ = false;
};

llvm::CodeGenOptLevel toLLVMOptimization(
    NativeOptimizationLevel level
) {
    switch (level) {
        case NativeOptimizationLevel::None:
            return llvm::CodeGenOptLevel::None;

        case NativeOptimizationLevel::Less:
            return llvm::CodeGenOptLevel::Less;

        case NativeOptimizationLevel::Default:
            return llvm::CodeGenOptLevel::Default;

        case NativeOptimizationLevel::Agressive:
            return static_cast<llvm::CodeGenOptLevel>(3);
    }

    return llvm::CodeGenOptLevel::Default;
}

} // namespace

NativeCompileResult LLVMBackend::emitObjectFile(
    const CompiledProgram& program,
    const std::filesystem::path& output,
    const LLVMBackendOptions& options
) const {
    NativeCompileResult result;

    if (!program.program_) {
        result.diagnostics.push_back(
            Diagnostic{
                DiagnosticSeverity::Error,
                "CompileError",
                "Cannot generate native code from an empty CompiledProgram",
                {},
                {}
            }
        );

        return result;
    }

    try {
        std::error_code filesystemError;

        if (!output.parent_path().empty()) {
            std::filesystem::create_directories(
                output.parent_path(),
                filesystemError
            );
        }

        if (filesystemError) {
            result.diagnostics.push_back(
                Diagnostic{
                    DiagnosticSeverity::Error,
                    "CompileError",
                    "Cannot create output directory: " +
                        filesystemError.message(),
                    {},
                    {}
                }
            );

            return result;
        }

        static const bool targetInitialized = [] {
            return !llvm::InitializeNativeTarget() &&
                   !llvm::InitializeNativeTargetAsmPrinter() &&
                   !llvm::InitializeNativeTargetAsmParser();
        }();

        if (!targetInitialized) {
            result.diagnostics.push_back(
                Diagnostic{
                    DiagnosticSeverity::Error,
                    "CompileError",
                    "LLVM could not initialize the host target. Check that the native LLVM backend is linked",
                    {},
                    {}
                }
            );

            return result;
        }

        const std::string tripleText =
            options.targetTriple.empty()
                ? llvm::sys::getDefaultTargetTriple()
                : options.targetTriple;

        const llvm::Triple triple(tripleText);

        std::string targetError;

        const auto* target =
            llvm::TargetRegistry::lookupTarget(
                triple,
                targetError
            );

        if (!target) {
            result.diagnostics.push_back(
                Diagnostic{
                    DiagnosticSeverity::Error,
                    "CompileError",
                    "LLVM cannot find target '" +
                        tripleText + "': " + targetError,
                    {},
                    {}
                }
            );

            return result;
        }

        llvm::TargetOptions targetOptions;

        std::unique_ptr<llvm::TargetMachine> targetMachine(
            target->createTargetMachine(
                triple,
                options.cpu,
                "",
                targetOptions,
                std::nullopt,
                std::nullopt,
                toLLVMOptimization(options.optimization)
            )
        );

        if (!targetMachine) {
            result.diagnostics.push_back(
                Diagnostic{
                    DiagnosticSeverity::Error,
                    "CompileError",
                    "LLVM could not create a target machine for '" +
                        tripleText + "'",
                    {},
                    {}
                }
            );

            return result;
        }

        llvm::LLVMContext context;

        ModuleGenerator generator(
            context,
            *program.program_
        );

        if (!generator.generate()) {
            result.diagnostics = generator.diagnostics();
            return result;
        }

        auto module = generator.takeModule();

        module->setTargetTriple(triple);
        module->setDataLayout(
            targetMachine->createDataLayout()
        );

        std::string verificationMessage;

        llvm::raw_string_ostream verificationStream(
            verificationMessage
        );

        if (
            llvm::verifyModule(
                *module,
                &verificationStream
            )
        ) {
            verificationStream.flush();

            result.diagnostics.push_back(
                Diagnostic{
                    DiagnosticSeverity::Error,
                    "CompileError",
                    "Generated LLVM IR failed verification: " +
                        verificationMessage,
                    {},
                    {}
                }
            );

            return result;
        }

        std::error_code outputError;

        llvm::raw_fd_ostream destination(
            output.string(),
            outputError,
            llvm::sys::fs::OF_None
        );

        if (outputError) {
            result.diagnostics.push_back(
                Diagnostic{
                    DiagnosticSeverity::Error,
                    "CompileError",
                    "Cannot open object-file output '" +
                        output.string() + "': " +
                        outputError.message(),
                    {},
                    {}
                }
            );

            return result;
        }

        llvm::legacy::PassManager passManager;

        if (
            targetMachine->addPassesToEmitFile(
                passManager,
                destination,
                nullptr,
                llvm::CodeGenFileType::ObjectFile
            )
        ) {
            result.diagnostics.push_back(
                Diagnostic{
                    DiagnosticSeverity::Error,
                    "CompileError",
                    "The selected LLVM target cannot emit object files",
                    {},
                    {}
                }
            );

            return result;
        }

        passManager.run(*module);
        destination.flush();

        if (destination.has_error()) {
            result.diagnostics.push_back(
                Diagnostic{
                    DiagnosticSeverity::Error,
                    "CompileError",
                    "LLVM encountered an error while writing the object file",
                    {},
                    {}
                }
            );

            return result;
        }

        result.succeeded = true;
        result.objectFile = output;

        return result;
    } catch (const std::exception& exception) {
        result.diagnostics.push_back(
            Diagnostic{
                DiagnosticSeverity::Error,
                "FatalError",
                std::string(
                    "Unexpected LLVM backend failure: "
                ) + exception.what(),
                {},
                {}
            }
        );

        return result;
    }
}

} // namespace cmath
