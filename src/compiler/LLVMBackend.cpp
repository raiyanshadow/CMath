
#include "cmath/compiler/LLVMBackend.h"

#include "LanguageInternal.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
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
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cmath {
namespace {

using language::detail::IRDeclaration;
using language::detail::IRAssignment;
using language::detail::IRExpression;
using language::detail::IRExpressionStatement;
using language::detail::IRExprPtr;
using language::detail::IRFunction;
using language::detail::IRStatement;
using language::detail::IRReturn;
using language::detail::IRIf;
using language::detail::IRWhile;
using language::detail::IRFor;
using language::detail::IRBreak;
using language::detail::IRContinue;
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
        auto* currentBlock = builder_.GetInsertBlock();

        if (!currentBlock)
            return nullptr;

        auto* function = currentBlock->getParent();
        auto& entryBlock = function->getEntryBlock();

        llvm::IRBuilder<> entryBuilder(
            &entryBlock, entryBlock.begin()
        );


        return entryBuilder.CreateAlloca(type, nullptr, name+".addr");
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
                    emitAbortIf(overflow, expression->location, "Integer increment/decrement overflow");
                    updated = increment ? builder_.CreateAdd(previous, builder_.getInt64(1), "integer.increment") : builder_.CreateSub(previous, builder_.getInt64(1), "integer.decrement");
                } else if (found->second.typeKind == ValueType::Real) {
                    const auto one = llvm::ConstantFP::get(builder_.getDoubleTy(), 1.0);
                    updated = increment ? builder_.CreateFAdd(previous, one, "real.increment") : builder_.CreateFSub(previous, one, "real.decrement");

                    const auto maximum = llvm::ConstantFP::get(builder_.getDoubleTy(), std::numeric_limits<double>::max());
                    const auto minimum = llvm::ConstantFP::get(builder_.getDoubleTy(), -std::numeric_limits<double>::max());

                    auto *finiteUpper = builder_.CreateFCmpOLE(updated, maximum);
                    auto *finiteLower = builder_.CreateFCmpOGE(updated, minimum);
                    auto *finite = builder_.CreateAnd(finiteUpper, finiteLower);

                    emitAbortIf(builder_.CreateNot(finite), expression->location, "Real increment/decrement produced a non-finite value");
                } else {
                    error(expression->location, "FeatuerError", "Realtive increment/decrement currently supports Integer and Real variables");
                    return nullptr;
                }

                builder_.CreateStore(updated, found->second.address);
                llvm::Value* result = expression->postfix ? previous : updated;

                return coerce(result, found->second.typeKind, effectiveType, expression->location);
            }

            case IRExpression::Kind::Unary: {
                if (expression->children.size() != 1 || !expression->children.front()) {
                    error(expression->location, "CompileError", "Malformed unary expression in native code generation");
                    return nullptr;
                }

                const auto& operandExpression = expression->children.front();

                auto* operand = effectiveType == ValueType::Real ? emitRealExpression(operandExpression) : emitExpression(operandExpression);
                const auto operandType = operandExpression->type;

                if (!operand)
                    return nullptr;

                if (expression->text == "not") {
                    if (!operand->getType()->isIntegerTy(1)) {
                        error(expression->location, "TypeError", "'not' requires a Boolean operand");
                        return nullptr;
                    }

                    return builder_.CreateNot(operand, "boolean.not");
                }

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

    llvm::Value* emitShortCircuit(const IRExprPtr& expression) {
        if (!expression || expression->children.size() != 2 || !expression->children[0] || !expression->children[1]) {
            error(expression ? expression->location : SourceLocation{}, "CompileError", "Malformed logical expression in native code generation");
            return nullptr;
        }

        const bool isAnd = expression->text == "and";
        auto *left = emitExpression(expression->children[0]);

        if (!left) return nullptr;

        if (!left->getType()->isIntegerTy(1)) {
            error(expression->location, "TypeError", "Logical operations require Boolean operands");
            return nullptr;
        }

        auto *function = builder_.GetInsertBlock()->getParent();

        auto *shortCircuitBlock = llvm::BasicBlock::Create(context_, isAnd ? "and.short" : "or.short", function);

        auto *rightBlock = llvm::BasicBlock::Create(context_, isAnd ? "and.rhs" : "or.rhs", function);
        auto *mergeBlock = llvm::BasicBlock::Create(context_, isAnd ? "and.end" : "or.end", function);

        if (isAnd) {
            builder_.CreateCondBr(left, rightBlock, shortCircuitBlock);
        } else {
            builder_.CreateCondBr(left, shortCircuitBlock, rightBlock);
        }

        builder_.SetInsertPoint(shortCircuitBlock);
        builder_.CreateBr(mergeBlock);

        builder_.SetInsertPoint(rightBlock);
        auto *right = emitExpression(expression->children[1]);

        if (!right) {
            return nullptr;
        }

        if (!right->getType()->isIntegerTy(1)) {
            error(expression->location, "TypeError", "Logical operations require Boolean operands");
            return nullptr;
        }

        auto *rightEndBlock = builder_.GetInsertBlock();

        if (!rightEndBlock || rightEndBlock->getTerminatorOrNull()) {
            error(expression->location, "CompileError", "Logical right operand did not produce a continuing control-flow path");
            return nullptr;
        }

        builder_.CreateBr(mergeBlock);

        builder_.SetInsertPoint(mergeBlock);
        auto *result = builder_.CreatePHI(builder_.getInt1Ty(), 2, isAnd ? "and.result" : "or.result");
        result->addIncoming(llvm::ConstantInt::get(builder_.getInt1Ty(), isAnd ? 0 : 1), shortCircuitBlock);
        result->addIncoming(right, rightEndBlock);

        return result;
    }

    llvm::Value* emitBinary(
        const IRExprPtr& expression,
        ValueType expected
    ) {
        if (!expression || expression->children.size() != 2 || !expression->children[0] || !expression->children[1]) {
            error(expression ? expression->location : SourceLocation{}, "CompileError", "Malformed binary expression in native code generation");
            return nullptr;
        }

        const auto& leftExpr = expression->children[0];
        const auto& rightExpr = expression->children[1];
        const std::string& op = expression->text;

        if (op == "and" || op == "or")
            return emitShortCircuit(expression);

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
                // Integer exponentiation requires a nonnegative exponent.
                auto* negativeExponent = builder_.CreateICmpSLT(
                    right,
                    builder_.getInt64(0),
                    "integer.power.negative"
                );

                emitAbortIf(
                    negativeExponent,
                    expression->location,
                    "Negative Integer exponent"
                );

                auto* function = builder_.GetInsertBlock()->getParent();

                auto* preheader = builder_.GetInsertBlock();

                auto* loopBlock = llvm::BasicBlock::Create(
                    context_,
                    "integer.power.loop",
                    function
                );

                auto* bodyBlock = llvm::BasicBlock::Create(
                    context_,
                    "integer.power.body",
                    function
                );

                auto* exitBlock = llvm::BasicBlock::Create(
                    context_,
                    "integer.power.exit",
                    function
                );

                auto* multiplyWithOverflow = llvm::Intrinsic::getOrInsertDeclaration(
                    module_.get(),
                    llvm::Intrinsic::smul_with_overflow,
                    {builder_.getInt64Ty()}
                );

                const auto one = builder_.getInt64(1);
                const auto zero = builder_.getInt64(0);

                // Enter the exponentiation loop.
                builder_.CreateBr(loopBlock);

                builder_.SetInsertPoint(loopBlock);

                auto* resultPhi = builder_.CreatePHI(
                    builder_.getInt64Ty(),
                    2,
                    "integer.power.result"
                );

                auto* basePhi = builder_.CreatePHI(
                    builder_.getInt64Ty(),
                    2,
                    "integer.power.base"
                );

                auto* exponentPhi = builder_.CreatePHI(
                    builder_.getInt64Ty(),
                    2,
                    "integer.power.exponent"
                );

                resultPhi->addIncoming(one, preheader);
                basePhi->addIncoming(left, preheader);
                exponentPhi->addIncoming(right, preheader);

                auto* finished = builder_.CreateICmpEQ(
                    exponentPhi,
                    zero,
                    "integer.power.finished"
                );

                builder_.CreateCondBr(
                    finished,
                    exitBlock,
                    bodyBlock
                );

                // Multiply the result when the current exponent bit is set.
                builder_.SetInsertPoint(bodyBlock);

                auto* resultProductPair = builder_.CreateCall(
                    multiplyWithOverflow,
                    {resultPhi, basePhi},
                    "integer.power.product.checked"
                );

                auto* resultProduct = builder_.CreateExtractValue(
                    resultProductPair,
                    {0},
                    "integer.power.product"
                );

                auto* resultOverflow = builder_.CreateExtractValue(
                    resultProductPair,
                    {1},
                    "integer.power.overflow"
                );

                auto* exponentIsOdd = builder_.CreateICmpNE(
                    builder_.CreateAnd(exponentPhi, one),
                    zero,
                    "integer.power.odd"
                );

                auto* resultOverflowUsed = builder_.CreateAnd(
                    exponentIsOdd,
                    resultOverflow
                );

                emitAbortIf(
                    resultOverflowUsed,
                    expression->location,
                    "Integer exponentiation overflow"
                );

                auto* nextResult = builder_.CreateSelect(
                    exponentIsOdd,
                    resultProduct,
                    resultPhi,
                    "integer.power.next.result"
                );

                // Shift the exponent to process its next bit.
                auto* nextExponent = builder_.CreateLShr(
                    exponentPhi,
                    one,
                    "integer.power.shift"
                );

                // Square the base for the next iteration.
                auto* baseProductPair = builder_.CreateCall(
                    multiplyWithOverflow,
                    {basePhi, basePhi},
                    "integer.power.square.checked"
                );

                auto* baseProduct = builder_.CreateExtractValue(
                    baseProductPair,
                    {0},
                    "integer.power.square"
                );

                auto* baseOverflow = builder_.CreateExtractValue(
                    baseProductPair,
                    {1},
                    "integer.power.square.overflow"
                );

                auto* needsSquare = builder_.CreateICmpNE(
                    nextExponent,
                    zero,
                    "integer.power.needs.square"
                );

                auto* baseOverflowUsed = builder_.CreateAnd(
                    needsSquare,
                    baseOverflow
                );

                emitAbortIf(
                    baseOverflowUsed,
                    expression->location,
                    "Integer exponentiation overflow"
                );

                // The current insertion block may now be an overflow-check
                // continuation block.
                auto* bodyEnd = builder_.GetInsertBlock();

                builder_.CreateBr(loopBlock);

                resultPhi->addIncoming(nextResult, bodyEnd);
                basePhi->addIncoming(baseProduct, bodyEnd);
                exponentPhi->addIncoming(nextExponent, bodyEnd);

                // Return the accumulated result.
                builder_.SetInsertPoint(exitBlock);

                return resultPhi;
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

    bool currentBlockTerminated() const {
        auto* block = builder_.GetInsertBlock();
        return !block || block->getTerminatorOrNull() != nullptr;
    }

    bool emitBlock(const language::detail::IRBlock& block, bool scoped = true) {
        auto previousVariables = variables_;

        for (const auto & statement : block.statements) {
            if (currentBlockTerminated()) {
                break;
            }
            if (!emitStatement(statement)) {
                if (scoped) variables_ = std::move(previousVariables);
                return false;
            }
        }

        if (scoped) variables_ = std::move(previousVariables);
        return diagnostics_.empty();
    }

    bool emitIf(const language::detail::IRIf& conditional, const SourceLocation& location) {
        auto *function = builder_.GetInsertBlock()->getParent();

        auto *mergeBlock = llvm::BasicBlock::Create(context_, "if.end", function);

        bool hasMergePredecessor = false;
        for (const auto& branch : conditional.branches) {
            if (!branch.condition || !branch.body) {
                error(location, "CompileError", "Malformed conditional branch in native code generation");
                return false;
            }

            auto *condition = emitExpression(branch.condition);

            if (!condition)
                return false;

            if (!condition->getType()->isIntegerTy(1)) {
                error(location, "TypeError", "An if/elif condition must evaluate to Boolean");
                return false;
            }

            auto *bodyBlock = llvm::BasicBlock::Create(context_, "if.branch", function);

            auto *nextConditionBlock = llvm::BasicBlock::Create(context_, "if.next", function);
            builder_.CreateCondBr(condition, bodyBlock, nextConditionBlock);
            builder_.SetInsertPoint(bodyBlock);

            if (!emitBlock(*branch.body)) return false;

            if (!currentBlockTerminated()) {
                builder_.CreateBr(mergeBlock);
                hasMergePredecessor = true;
            }

            builder_.SetInsertPoint(nextConditionBlock);
        }

        if (conditional.elseBody) {
            if (!emitBlock(*conditional.elseBody)) return false;
            if (!currentBlockTerminated()) {
                builder_.CreateBr(mergeBlock);
                hasMergePredecessor = true;
            }
        } else {
            builder_.CreateBr(mergeBlock);
            hasMergePredecessor = true;
        }

        builder_.SetInsertPoint(mergeBlock);

        if (!hasMergePredecessor) builder_.CreateUnreachable();
        
        return true;
    }  
    
    bool emitWhile(const language::detail::IRWhile& loop, const SourceLocation& location) {
        if (!loop.condition || !loop.body) {
            error(location, "CompileError", "Malformed while loop in native code generation");
            return false;
        }

        auto *function = builder_.GetInsertBlock()->getParent();
        auto *conditionBlock = llvm::BasicBlock::Create(context_, "while.condition", function);
        auto *bodyBlock = llvm::BasicBlock::Create(context_, "while.body", function);

        auto *exitBlock = llvm::BasicBlock::Create(context_, "while.end", function);
        builder_.CreateBr(conditionBlock);
        builder_.SetInsertPoint(conditionBlock);

        auto *condition = emitExpression(loop.condition);
        if (!condition) return false;

        if (!condition->getType()->isIntegerTy(1)) {
            error(location, "TypeError", "A while condition must evaluate to Boolean");
            return false;
        }

        builder_.CreateCondBr(condition, bodyBlock, exitBlock);
        builder_.SetInsertPoint(bodyBlock);
        loopTargets_.push_back(LoopTargets{exitBlock, conditionBlock});

        const bool bodySucceeded = emitBlock(*loop.body);
        loopTargets_.pop_back();

        if (!bodySucceeded) return false;

        if (!currentBlockTerminated()) builder_.CreateBr(conditionBlock);

        builder_.SetInsertPoint(exitBlock);
        return true;
    }

    bool emitFor(const language::detail::IRFor& loop, const SourceLocation& location) {
        if (!loop.start || !loop.end || !loop.body) {
            error(location, "CompileError", "Malformed for loop in native code generation");
            return false;
        }

        auto *startValue = emitExpression(loop.start);
        if (!startValue) return false;

        auto *endValue = emitExpression(loop.end);
        if (!endValue) return false;

        if (!startValue->getType()->isIntegerTy(64) || !endValue->getType()->isIntegerTy(64)) {
            error(location, "TypeError", "For-loop bounds must evaluate to Integer");
            return false;
        }

        auto *function = builder_.GetInsertBlock()->getParent();

        auto *iteratorAddress = createLocal(loop.iterator, builder_.getInt64Ty());

        if (!iteratorAddress) return false;

        builder_.CreateStore(startValue, iteratorAddress);
        const auto previousVariables = variables_;
        variables_[loop.iterator] = Variable{
            iteratorAddress,
            builder_.getInt64Ty(),
            ValueType::Integer
        };

        auto *conditionBlock = llvm::BasicBlock::Create(context_, "for.condition", function);
        auto *bodyBlock = llvm::BasicBlock::Create(context_, "for.body", function);
        auto *incrementBlock = llvm::BasicBlock::Create(context_, "for.increment", function);
        auto *exitBlock = llvm::BasicBlock::Create(context_, "for.end", function);

        builder_.CreateBr(conditionBlock);
        builder_.SetInsertPoint(conditionBlock);

        auto *currentIndex = builder_.CreateLoad(builder_.getInt64Ty(), iteratorAddress, loop.iterator + ".current");
        auto *condition = builder_.CreateICmpSLT(currentIndex, endValue, "for.condition.value");

        builder_.CreateCondBr(condition, bodyBlock, exitBlock);
        builder_.SetInsertPoint(bodyBlock);

        loopTargets_.push_back(LoopTargets{exitBlock, incrementBlock});
        const bool bodySucceeded = emitBlock(*loop.body);
        loopTargets_.pop_back();
        if (!bodySucceeded) {
            variables_ = previousVariables;
            return false;
        }

        if (!currentBlockTerminated()) builder_.CreateBr(incrementBlock);

        builder_.SetInsertPoint(incrementBlock);

        auto* oldIndex = builder_.CreateLoad(builder_.getInt64Ty(), iteratorAddress, loop.iterator + ".increment.old");

        auto* nextIndex = builder_.CreateAdd(oldIndex, builder_.getInt64(1), loop.iterator + ".increment");

        builder_.CreateStore(nextIndex, iteratorAddress);
        builder_.CreateBr(conditionBlock);

        builder_.SetInsertPoint(exitBlock);

        variables_ = previousVariables;
        return true;
    }


    bool emitStatement(
        const IRStatement& statement
    ) {
        if (currentBlockTerminated()) return true;
        
        return std::visit(
            [this, &statement](const auto &node) -> bool {
                using T = std::decay_t<decltype(node)>;

                if constexpr(std::is_same_v<T, IRDeclaration>) {
                    if (node.type == ValueType::Unit) {
                        error(statement.location, "CompileError", "Unit values cannot be stored in variables");
                        return false;
                    }

                    auto * type = llvmType(node.type);
                    if (!type) return false;
                    auto *value = emitExpression(node.initializer, node.type);
                    if (!value) return false;
                    auto *address = createLocal(node.name, type);
                    if (!address) return false;

                    builder_.CreateStore(value, address);
                    variables_[node.name] = Variable{address, type, node.type};

                    return true;
                } else if constexpr(std::is_same_v<T, IRAssignment>) {
                    const auto found = variables_.find(node.name);
                    if (found == variables_.end()) {
                        error(statement.location, "CompileError", "Assignment target '" + node.name + "' is not available in this native scope");
                        return false;
                    }

                    auto *value = emitExpression(node.expression, node.type);

                    if (!value) return false;

                    builder_.CreateStore(value, found->second.address);
                    return true;
                } else if constexpr(std::is_same_v<T, IRExpressionStatement>) {
                    return emitExpression(node.expression) != nullptr;
                } else if constexpr(std::is_same_v<T, IRReturn>) {
                    if (inMain_) {
                        if(node.expression && !emitExpression(node.expression)) {
                            return false;
                        }
                        builder_.CreateRet(builder_.getInt32(0));
                        return true;
                    }

                    if (node.type == ValueType::Unit) {
                        builder_.CreateRetVoid();
                        return true;
                    }

                    if (!node.expression) {
                        error(statement.location, "CompileError", "A non-Unit return requires a value");
                        return false;
                    }

                    auto *value = emitExpression(node.expression, node.type);

                    if (!value) return false;

                    builder_.CreateRet(value);
                    return true;
                } else if constexpr(std::is_same_v<T, IRIf>) {
                    return emitIf(node, statement.location);
                } else if constexpr(std::is_same_v<T, IRWhile>) {
                    return emitWhile(node, statement.location);
                } else if constexpr(std::is_same_v<T, IRFor>) {
                    return emitFor(node, statement.location);
                } else if constexpr(std::is_same_v<T, IRBreak>) {
                    if (loopTargets_.empty()) {
                        error(statement.location, "ControlFlowError", "'break' appeared outside a generated loop");
                        return false;
                    }

                    builder_.CreateBr(loopTargets_.back().breakBlock);
                    return true;
                } else if constexpr(std::is_same_v<T, IRContinue>) {
                    if (loopTargets_.empty()) {
                        error(statement.location, "ControlFlowError", "'continue' appeared outside a generated loop");
                        return false;
                    }
                    builder_.CreateBr(loopTargets_.back().continueBlock);
                    return true;
                }
            }, statement.data
        );
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
        loopTargets_.clear();
        inMain_ = false;

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

        if (!emitBlock(sourceFunction.body)) return false;
        if (!currentBlockTerminated()) {
            if (sourceFunction.returnType == ValueType::Unit) {
                builder_.CreateRetVoid();
            } else {
                error(sourceFunction.location, "CompileError", "Native function '" + sourceFunction.name + "' has a reachable path without a return value");
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
        loopTargets_.clear();
        inMain_ = true;

        if (!emitBlock(program_.body, false)) return false;

        if (!currentBlockTerminated()) builder_.CreateRet(builder_.getInt32(0));

        inMain_ = false;

        if (!diagnostics_.empty()) return false;
        std::string verificationMessage;
        llvm::raw_string_ostream verificationStream(verificationMessage);

        if (llvm::verifyFunction(*mainFunction, &verificationStream)) {
            verificationStream.flush();
            error({}, "CompileError", "Generated main failed verification: " + verificationMessage);
            return false;
        }

        return true;
    }

    struct Variable {
        llvm::AllocaInst* address = nullptr;
        llvm::Type* type = nullptr;
        ValueType typeKind = ValueType::Error;
    };

    struct LoopTargets{
        llvm::BasicBlock* breakBlock = nullptr;
        llvm::BasicBlock* continueBlock = nullptr;
    };

    llvm::LLVMContext& context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    const ProgramIR& program_;
    std::vector<LoopTargets> loopTargets_;

    std::vector<Diagnostic> diagnostics_;

    std::unordered_map<std::string, Variable> variables_;
    std::unordered_map<std::string, llvm::Function*> userFunctions_;

    llvm::FunctionCallee printf_;
    llvm::FunctionCallee abort_;
    llvm::FunctionCallee strcmp_;
    llvm::FunctionCallee pow_;

    std::unordered_map<std::string, llvm::FunctionCallee>
        mathFunctions_;
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
