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
#include <numeric>
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
using language::detail::RationalValue;
using language::detail::ValueType;

bool checkedMul64(std::int64_t a, std::int64_t b, std::int64_t& result) {
    if (a == 0 || b == 0) {
        result = 0;
        return true;
    }

    if ((a == -1 && b == std::numeric_limits<std::int64_t>::min()) || 
        (b == -1 && a == std::numeric_limits<std::int64_t>::min())) {
            return false;
        }

    if (a > 0) {
        if (b > 0) {
            if (a > std::numeric_limits<std::int64_t>::max() / b) return false;
        } else {
            if (b < std::numeric_limits<std::int64_t>::min() / a) return false;
        }
    } else {
        if (b > 0) {
            if (a < std::numeric_limits<std::int64_t>::min() / b) return false;
        } else {
            if (a < std::numeric_limits<std::int64_t>::max() / b) return false;
        }
    }

    result = a * b;
    return true;
}

bool checkedAdd64(std::int64_t a, std::int64_t b, std::int64_t& result) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
            return false;
        }

        result = a + b;
        return true;
}

RationalValue parseRationalLiteral(const std::string& text) {
    const auto dot = text.find('.');

    if (dot == std::string::npos) {
        std::size_t consumed = 0;
        const auto numerator = std::stoll(text, &consumed);

        if (consumed != text.size()) 
            throw std::invalid_argument("Invalid Rational literal");

        return {numerator, 1};
    }

    const std::string wholeText = text.substr(0, dot);
    const std::string fractionalText = text.substr(dot+1);

    std::int64_t denominator = 1;

    for (std::size_t i = 0; i < fractionalText.size(); ++i) {
        if (!checkedMul64(denominator, 10, denominator)) {
            throw std::out_of_range("Rational literal exceeds the supported exact range");
        }
    }

    std::int64_t whole = 0;

    if (!wholeText.empty()) {
        std::size_t consumed = 0;
        whole = std::stoll(wholeText, &consumed);

        if (consumed != wholeText.size())
            throw std::invalid_argument("Invalid Rational literal");
    }

    std::int64_t fractional = 0;

    if (!fractionalText.empty()) {
        std::size_t consumed = 0;
        fractional = std::stoll(fractionalText, &consumed);

        if (consumed != fractionalText.size())
            throw std::invalid_argument("Invalid Rational literal");
    }

    std::int64_t scaledWhole = 0;
    std::int64_t numerator = 0;

    if (!checkedMul64(whole, denominator, scaledWhole) ||
        !checkedAdd64(scaledWhole, fractional, numerator)) {
        throw std::out_of_range("Rational literal exceeds the supported exact range");
    }

    const auto divisor = std::gcd(numerator, denominator);

    return {numerator/divisor, denominator/divisor};
}


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

    llvm::StructType* rationalLLVMType() {
        if (!rationalType_) {
            rationalType_ = llvm::StructType::create(context_, "cmath.Rational");
            rationalType_->setBody({builder_.getInt64Ty(), builder_.getInt64Ty()}, false);
        }

        return rationalType_;
    }

    llvm::Value* createRational(llvm::Value* numerator, llvm::Value* denominator) {
        auto* undefResult = llvm::UndefValue::get(rationalLLVMType());
        auto* result = builder_.CreateInsertValue(undefResult, numerator, {0}, "rational.numerator");
        return builder_.CreateInsertValue(result, denominator, {1}, "rational.denominator");
    }

    llvm::StructType* complexLLVMType() {
        if (!complexType_) {
            complexType_ = llvm::StructType::create(context_, "cmath.Complex");
            complexType_->setBody({builder_.getDoubleTy(), builder_.getDoubleTy()}, false);
        }
        return complexType_;
    }

    llvm::Value* createComplex(llvm::Value* real, llvm::Value* imaginary) {
        auto* undefResult = llvm::UndefValue::get(complexLLVMType());
        auto* result = builder_.CreateInsertValue(undefResult, real, {0}, "complex.real");
        return builder_.CreateInsertValue(result, imaginary, {1}, "complex.imaginary");
    }

    llvm::Function* rationalDivModFunction() {
        if (rationalDivMod_)
            return rationalDivMod_;
        
        auto* integerType = llvm::IntegerType::get(context_, 128);

        rationalDivModType_ = llvm::StructType::get(context_, {integerType, integerType});
        auto* functionType = llvm::FunctionType::get(rationalDivModType_, {integerType, integerType}, false);

        rationalDivMod_ = llvm::Function::Create(functionType, llvm::Function::InternalLinkage, "cmath.rational.divmod", module_.get());

        auto argument = rationalDivMod_->arg_begin();
        llvm::Value* dividend = &*argument++;
        llvm::Value* divisor = &*argument;

        auto* entry = llvm::BasicBlock::Create(context_, "entry", rationalDivMod_);
        auto* zeroBlock = llvm::BasicBlock::Create(context_, "divisor.zero", rationalDivMod_);
        auto* loopBlock = llvm::BasicBlock::Create(context_, "divmod.loop", rationalDivMod_);
        auto* exitBlock = llvm::BasicBlock::Create(context_, "divmod.exit", rationalDivMod_);

        llvm::IRBuilder<> helperBuilder(context_);

        auto* zero = llvm::ConstantInt::get(integerType, 0);
        auto* one = llvm::ConstantInt::get(integerType, 1);

        helperBuilder.SetInsertPoint(entry);

        auto* divisorIsZero = helperBuilder.CreateICmpEQ(divisor, zero);
        helperBuilder.CreateCondBr(divisorIsZero, zeroBlock, loopBlock);

        helperBuilder.SetInsertPoint(zeroBlock);
        auto* undefZeroResult = llvm::UndefValue::get(rationalDivModType_);
        auto* zeroResult = helperBuilder.CreateInsertValue(undefZeroResult, zero, {0});
        zeroResult = helperBuilder.CreateInsertValue(zeroResult, dividend, {1});
        helperBuilder.CreateRet(zeroResult);

        helperBuilder.SetInsertPoint(loopBlock);

        auto* bitIndex = helperBuilder.CreatePHI(helperBuilder.getInt32Ty(), 2, "divmod.bit.index");
        auto* quotient = helperBuilder.CreatePHI(integerType, 2, "divmod.quotient");
        auto* remainder = helperBuilder.CreatePHI(integerType, 2, "divmod.remainder");
        bitIndex->addIncoming(helperBuilder.getInt32(127), entry);

        quotient->addIncoming(zero, entry);
        remainder->addIncoming(zero, entry);

        auto* shiftAmount = helperBuilder.CreateZExt(bitIndex, integerType);
        auto* shiftedDividend = helperBuilder.CreateLShr(dividend, shiftAmount);

        auto* currentBit = helperBuilder.CreateAnd(shiftedDividend, one);
        auto* shiftedRemainder = helperBuilder.CreateShl(remainder, one);
        auto* candidateRemainder = helperBuilder.CreateOr(shiftedRemainder, currentBit);
        auto* canSubtract = helperBuilder.CreateICmpUGE(candidateRemainder, divisor);
        auto* subtractedRemainder = helperBuilder.CreateSub(candidateRemainder, divisor);
        auto* nextRemainder = helperBuilder.CreateSelect(canSubtract, subtractedRemainder, candidateRemainder);

        auto* bitMask = helperBuilder.CreateShl(one, shiftAmount);
        auto* updatedQuotient = helperBuilder.CreateOr(quotient, bitMask);
        auto* nextQuotient = helperBuilder.CreateSelect(canSubtract, updatedQuotient, quotient);
        
        auto* finished = helperBuilder.CreateICmpEQ(bitIndex, helperBuilder.getInt32(0));
        auto* nextBitIndex = helperBuilder.CreateSub(bitIndex, helperBuilder.getInt32(1));

        helperBuilder.CreateCondBr(finished, exitBlock, loopBlock);

        bitIndex->addIncoming(nextBitIndex, loopBlock);
        quotient->addIncoming(nextQuotient, loopBlock);
        remainder->addIncoming(nextRemainder, loopBlock);

        helperBuilder.SetInsertPoint(exitBlock);

        auto* undefResult = llvm::UndefValue::get(rationalDivModType_);
        auto* result = helperBuilder.CreateInsertValue(undefResult, nextQuotient, {0});
        result = helperBuilder.CreateInsertValue(result, nextRemainder, {1});
        helperBuilder.CreateRet(result);

        return rationalDivMod_;
    }

    llvm::Function* rationalGcdFunction() {
        if (rationalGcd_)
            return rationalGcd_;
        
        auto* integerType = llvm::IntegerType::get(context_, 128);

        auto* functionType = llvm::FunctionType::get(integerType, {integerType, integerType}, false);

        rationalGcd_ = llvm::Function::Create(functionType, llvm::GlobalValue::InternalLinkage, "cmath.rational.gcd", module_.get());

        auto argument = rationalGcd_->arg_begin();

        llvm::Value* initialA = &*argument++;
        llvm::Value* initialB = &*argument;

        auto* entry = llvm::BasicBlock::Create(context_, "entry", rationalGcd_);
        auto* loop = llvm::BasicBlock::Create(context_, "gcd.loop", rationalGcd_);
        auto* step = llvm::BasicBlock::Create(context_, "gcd.step", rationalGcd_);
        auto* exit = llvm::BasicBlock::Create(context_, "gcd.exit", rationalGcd_);

        llvm::IRBuilder<> helperBuilder(context_);

        helperBuilder.SetInsertPoint(entry);
        helperBuilder.CreateBr(loop);

        helperBuilder.SetInsertPoint(loop);

        auto* a = helperBuilder.CreatePHI(integerType, 2, "gcd.a");

        auto* b = helperBuilder.CreatePHI(integerType, 2, "gcd.b");

        a->addIncoming(initialA, entry);
        b->addIncoming(initialB, entry);

        auto* finished = helperBuilder.CreateICmpEQ(b, llvm::ConstantInt::get(integerType, 0));

        helperBuilder.CreateCondBr(finished, exit, step);
        helperBuilder.SetInsertPoint(step);

        auto* division = helperBuilder.CreateCall(rationalDivModFunction(), {a, b}, "gcd.division");
        auto* remainder = helperBuilder.CreateExtractValue(division, {1}, "gcd.remainder");

        helperBuilder.CreateBr(loop);

        a->addIncoming(b, step);
        b->addIncoming(remainder, step);

        helperBuilder.SetInsertPoint(exit);
        helperBuilder.CreateRet(a);

        return rationalGcd_;
    }

    llvm::Value* emitRationalNormalize(llvm::Value* numerator, llvm::Value* denominator, const SourceLocation& location) {
        auto* integerType = llvm::IntegerType::get(context_, 128);

        auto* zero = llvm::ConstantInt::get(integerType, 0);
        auto* denominatorIsZero = builder_.CreateICmpEQ(denominator, zero);

        emitAbortIf(denominatorIsZero, location, "Rational division by zero");

        auto* denominatorIsNegative = builder_.CreateICmpSLT(denominator, zero);
        auto* negativeNumerator = builder_.CreateNeg(numerator);
        auto* negativeDenominator = builder_.CreateNeg(denominator);

        numerator = builder_.CreateSelect(denominatorIsNegative, negativeNumerator, numerator);
        denominator = builder_.CreateSelect(denominatorIsNegative, negativeDenominator, denominator);

        auto* numeratorIsNegative = builder_.CreateICmpSLT(numerator, zero);
        auto* absoluteNumerator = builder_.CreateSelect(numeratorIsNegative, builder_.CreateNeg(numerator), numerator, "rational.absolute.numerator");
        auto* divisor = builder_.CreateCall(rationalGcdFunction(), {absoluteNumerator, denominator}, "rational.gcd");

        auto* numeratorDivision = builder_.CreateCall(rationalDivModFunction(), {absoluteNumerator, divisor}, "rational.numerator.division");
        auto* reducedAbsoluteNumerator = builder_.CreateExtractValue(numeratorDivision, {0}, "rational.reduced.absolute.numerator");
        auto* reducedNumerator = builder_.CreateSelect(numeratorIsNegative, builder_.CreateNeg(reducedAbsoluteNumerator), reducedAbsoluteNumerator, "rational.reduced.numerator");
        
        auto* denominatorDivision = builder_.CreateCall(rationalDivModFunction(), {denominator, divisor}, "rational.denominator.division");
        auto* reducedDenominator = builder_.CreateExtractValue(denominatorDivision, {0}, "rational.reduced.denominator");

        const auto minimum = llvm::ConstantInt::getSigned(integerType, std::numeric_limits<std::int64_t>::min());
        const auto maximum = llvm::ConstantInt::getSigned(integerType, std::numeric_limits<std::int64_t>::max());

        auto* numeratorTooSmall = builder_.CreateICmpSLT(reducedNumerator, minimum);
        auto* numeratorTooLarge = builder_.CreateICmpSGT(reducedNumerator, maximum);
        auto* denominatorTooLarge = builder_.CreateICmpUGT(reducedDenominator, maximum);

        auto* invalid = builder_.CreateOr(numeratorTooSmall, builder_.CreateOr(numeratorTooLarge, denominatorTooLarge));

        emitAbortIf(invalid, location, "Rational arithmetic overflows the supported 64-bit range");
        auto* numerator64 = builder_.CreateTrunc(reducedNumerator, builder_.getInt64Ty(), "rational.numerator.i64");
        auto* denominator64 = builder_.CreateTrunc(reducedDenominator, builder_.getInt64Ty(), "rational.denominator.i64");
        return createRational(numerator64, denominator64);
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
                return rationalLLVMType();

            case ValueType::Complex:
                return complexLLVMType();

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

        fflush_ = declareFunction(
            "fflush",
            builder_.getInt32Ty(),
            {builder_.getPtrTy()}
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

        mathFunctions_.emplace("hypot", declareFunction("hypot", doubleType, {doubleType, doubleType}));
        mathFunctions_.emplace("atan2", declareFunction("atan2", doubleType, {doubleType, doubleType}));

        pow_ = declareFunction(
            "pow",
            doubleType,
            {doubleType, doubleType}
        );
    }

    bool declareUserFunctions() {
        for (const auto& [name, function] : program_.functions) {
            if (function.returnTypeInfo.isFunction()) {
                error(function.location, "FeatureError", "Native compilation of function returning function value is not supported yet.");
                return false;
            }
            std::vector<llvm::Type*> parameters;
            parameters.reserve(function.parameters.size());

            for (const auto& parameter : function.parameters) {
                if (parameter.type == ValueType::Unit) {
                    error(
                        function.location,
                        "TypeError",
                        "Function parameters cannot have type Unit."
                    );
                    return false;
                }

                if (parameter.typeInfo.isFunction()) {
                    error(function.location, "FeatureError", "Native compilation of function-valued parameters is not supported yet");
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

        if (to == ValueType::Rational && from == ValueType::Integer) {
            return createRational(value, builder_.getInt64(1));
        }

        if (to == ValueType::Real && from == ValueType::Integer) {
            return builder_.CreateSIToFP(
                value,
                builder_.getDoubleTy(),
                "int.to.real"
            );
        }

        if (to == ValueType::Real && from == ValueType::Rational) {
            auto* numerator = builder_.CreateExtractValue(value, {0});
            auto* denominator = builder_.CreateExtractValue(value, {1});

            auto* numeratorReal = builder_.CreateSIToFP(numerator, builder_.getDoubleTy());
            auto* denominatorReal = builder_.CreateSIToFP(denominator, builder_.getDoubleTy());

            return builder_.CreateFDiv(numeratorReal, denominatorReal, "rational.to.real");
        }

        if (to == ValueType::Complex) {
            llvm::Value* realPart = nullptr;

            if (from == ValueType::Integer) {
                realPart = builder_.CreateSIToFP(value, builder_.getDoubleTy(), "integer.to.complex");
            } else if (from == ValueType::Rational) {
                auto* numerator = builder_.CreateExtractValue(value, {0});
                auto* denominator = builder_.CreateExtractValue(value, {1});

                auto* numeratorReal = builder_.CreateSIToFP(numerator, builder_.getDoubleTy());
                auto* denominatorReal = builder_.CreateSIToFP(denominator, builder_.getDoubleTy());
                realPart = builder_.CreateFDiv(numeratorReal, denominatorReal, "rational.to.complex");
            } else if (from == ValueType::Real) {
                realPart = value;
            }

            if (realPart) {
                auto* imaginaryPart = llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0);
                return createComplex(realPart, imaginaryPart);
            }
        }

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

                if (expression->type == ValueType::Rational) {
                    try {
                        const auto rational = parseRationalLiteral(expression->text);
                        auto* constant = llvm::ConstantStruct::get(rationalLLVMType(), {
                            llvm::ConstantInt::getSigned(builder_.getInt64Ty(), rational.numerator),
                            llvm::ConstantInt::getSigned(builder_.getInt64Ty(), rational.denominator)
                        });

                        return coerce(constant, ValueType::Rational, effectiveType, expression->location);
                    } catch (...) {
                        error(
                            expression->location,
                            "CompileError",
                            "Rational literal exceeds the supported exact 64-bit range"
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

                        auto* constant = llvm::ConstantFP::get(builder_.getDoubleTy(), value);
                        return coerce(constant, ValueType::Real, effectiveType, expression->location);
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
                    return coerce(llvm::ConstantFP::get(builder_.getDoubleTy(), 3.14159265358979323846), ValueType::Real, effectiveType, expression->location);
                }

                if (expression->text == "e") {
                    return coerce(llvm::ConstantFP::get(
                        builder_.getDoubleTy(),
                        2.71828182845904523536
                    ), ValueType::Real, effectiveType, expression->location);
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
                    return coerce(loaded, found->second.typeKind, effectiveType, expression->location);
                }

                return loaded;
            }

            case IRExpression::Kind::ComplexImaginary:
                return createComplex(llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0), llvm::ConstantFP::get(builder_.getDoubleTy(), 1.0));

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

                if (expected == ValueType::Complex && expression->type != ValueType::Complex && expression->type != ValueType::Error) {
                    auto* value = emitExpression(expression);
                    return coerce(value, expression->type, ValueType::Complex, expression->location);
                }

                llvm::Value* operand = nullptr;

                if (effectiveType == ValueType::Real) {
                    operand = emitRealExpression(operandExpression);
                } else if (effectiveType == ValueType::Complex) {
                    operand = emitExpression(operandExpression, ValueType::Complex);
                } else {
                    operand = emitExpression(operandExpression);
                }

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
                    if (effectiveType == ValueType::Complex) {
                        auto* realPart = builder_.CreateExtractValue(operand, {0}, "complex.negate.real");
                        auto* imaginaryPart = builder_.CreateExtractValue(operand, {1}, "complex.negate.imaginary");
                        return createComplex(builder_.CreateFNeg(realPart, "complex.negate.real.result"), builder_.CreateFNeg(imaginaryPart, "complex.negate.imaginary.result"));
                    }

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

                    if (operandType == ValueType::Rational && effectiveType == ValueType::Rational) {
                        auto* numerator = builder_.CreateExtractValue(operand, {0}, "rational.negate.numerator");
                        auto* denominator = builder_.CreateExtractValue(operand, {1}, "rational.negate.denominator");
                        auto* minimum = llvm::ConstantInt::getSigned(builder_.getInt64Ty(), std::numeric_limits<std::int64_t>::min());
                        auto* overflows = builder_.CreateICmpEQ(numerator, minimum, "rational.negate.overflow");

                        emitAbortIf(overflows, expression->location, "Rational negation overflows the supported 64-bit range");

                        auto* negatedNumerator = builder_.CreateNeg(numerator, "rational.negate");
                        return createRational(negatedNumerator, denominator);
                    }
                }

                error(
                    expression->location,
                    "CompileError",
                    "Unsupported unary operation in native code generation"
                );

                return nullptr;
            }

            case IRExpression::Kind::Binary: {
                if (expected == ValueType::Complex && expression->type != ValueType::Complex && expression->type != ValueType::Error) {
                    auto* value = emitBinary(expression, ValueType::Error);
                    return coerce(value, expression->type, ValueType::Complex, expression->location);
                }
                return emitBinary(expression, expected);
            }

            case IRExpression::Kind::Call: {
                if (expression->indirectCall) {
                    error(expression->location, "FeatureError", "Native compilation of indirect function calls is not supported yet");
                    return nullptr;
                }

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
                if (op != "==" && op != "!=") {
                    error(expression->location, "DomainError", "Complex numbers support equality comparisons only");
                    return nullptr;
                }
                auto* left = emitExpression(leftExpr, ValueType::Complex);
                auto* right = emitExpression(rightExpr, ValueType::Complex);

                if (!left || !right) return nullptr;

                auto* leftReal = builder_.CreateExtractValue(left, {0});
                auto* leftImaginary = builder_.CreateExtractValue(left, {1});

                auto* rightReal = builder_.CreateExtractValue(right, {0});
                auto* rightImaginary = builder_.CreateExtractValue(right, {1});

                auto* realEqual = builder_.CreateFCmpOEQ(leftReal, rightReal);
                auto* imaginaryEqual = builder_.CreateFCmpOEQ(leftImaginary, rightImaginary);

                if (op == "==") {
                    return builder_.CreateAnd(realEqual, imaginaryEqual, "complex.equal");
                }

                auto* realNotEqual = builder_.CreateFCmpUNE(leftReal, rightReal);
                auto* imaginaryNotEqual = builder_.CreateFCmpUNE(leftImaginary, rightImaginary);

                return builder_.CreateOr(realNotEqual, imaginaryNotEqual, "complex.not.equal");
            }

            const bool useReal =
                leftType == ValueType::Real ||
                rightType == ValueType::Real;

            if (!useReal && (leftType == ValueType::Rational || rightType == ValueType::Rational)) {
                auto* left = emitExpression(leftExpr, ValueType::Rational);
                auto* right = emitExpression(rightExpr, ValueType::Rational);
                if (!left || !right) 
                    return nullptr;

                auto* leftNumerator = builder_.CreateSExt(builder_.CreateExtractValue(left, {0}), llvm::IntegerType::get(context_, 128));
                auto* leftDenominator = builder_.CreateSExt(builder_.CreateExtractValue(left, {1}), llvm::IntegerType::get(context_, 128));

                auto* rightNumerator = builder_.CreateSExt(builder_.CreateExtractValue(right, {0}), llvm::IntegerType::get(context_, 128));
                auto* rightDenominator = builder_.CreateSExt(builder_.CreateExtractValue(right, {1}), llvm::IntegerType::get(context_, 128));

                auto* leftCrossProduct = builder_.CreateMul(leftNumerator, rightDenominator);
                auto* rightCrossProduct = builder_.CreateMul(rightNumerator, leftDenominator);

                if (op == "==")
                    return builder_.CreateICmpEQ(leftCrossProduct, rightCrossProduct);
                if (op == "!=")
                    return builder_.CreateICmpNE(leftCrossProduct, rightCrossProduct);
                if (op == "<")
                    return builder_.CreateICmpSLT(leftCrossProduct, rightCrossProduct);
                if (op == ">")
                    return builder_.CreateICmpSGT(leftCrossProduct, rightCrossProduct);
                if (op == "<=")
                    return builder_.CreateICmpSLE(leftCrossProduct, rightCrossProduct);
                
                return builder_.CreateICmpSGE(leftCrossProduct, rightCrossProduct);
            }

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

        if (expression->type == ValueType::Complex) {

            auto* left = emitExpression(leftExpr, ValueType::Complex);
            auto* right = emitExpression(rightExpr, ValueType::Complex);
            if (!left || !right) return nullptr;

            auto* a = builder_.CreateExtractValue(left, {0}, "complex.left.real");
            auto* b = builder_.CreateExtractValue(left, {1}, "complex.left.imaginary");

            auto* c = builder_.CreateExtractValue(right, {0}, "complex.right.real");
            auto* d = builder_.CreateExtractValue(right, {1}, "complex.right.imaginary");

            if (op == "+") {
                return createComplex(builder_.CreateFAdd(a, c, "complex.add.real"), builder_.CreateFAdd(b, d, "complex.add.imaginary"));
            }

            if (op == "-") {
                return createComplex(builder_.CreateFSub(a, c, "complex.subtract.real"), builder_.CreateFSub(b, d, "complex.subtract.imaginary"));
            }

            if (op == "*") {
                auto* ac = builder_.CreateFMul(a, c);
                auto* bd = builder_.CreateFMul(b, d);
                auto* ad = builder_.CreateFMul(a, d);
                auto* bc = builder_.CreateFMul(b, c);

                return createComplex(builder_.CreateFSub(ac, bd, "complex.multiply.real"), builder_.CreateFAdd(ad, bc, "complex.multiply.imaginary"));
            }

            if (op == "/") {
                auto* zero = llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0);

                auto* cIsZero = builder_.CreateFCmpOEQ(c, zero);
                auto* dIsZero = builder_.CreateFCmpOEQ(d, zero);

                emitAbortIf(builder_.CreateAnd(cIsZero, dIsZero), expression->location, "Complex division by zero");

                // Smith's algorithm
                auto* absC = builder_.CreateCall(mathFunctions_.at("fabs"), {c}, "complex.division.abs.real");
                auto* absD = builder_.CreateCall(mathFunctions_.at("fabs"), {d}, "complex.divide.abs.imaginary");

                auto* realDominant = builder_.CreateFCmpOGE(absC, absD);

                auto* function = builder_.GetInsertBlock()->getParent();
                auto* realDominantBlock = llvm::BasicBlock::Create(context_, "complex.divide.real.dominant", function);
                auto* imaginaryDominantBlock = llvm::BasicBlock::Create(context_, "complex.divide.imaginary.dominant", function);
                auto* mergeBlock = llvm::BasicBlock::Create(context_, "complex.divide.merge", function);
                builder_.CreateCondBr(realDominant, realDominantBlock, imaginaryDominantBlock);

                builder_.SetInsertPoint(realDominantBlock);

                auto* ratioReal = builder_.CreateFDiv(d, c);
                auto* denominatorReal = builder_.CreateFAdd(c, builder_.CreateFMul(d, ratioReal));
                auto* resultRealA = builder_.CreateFDiv(builder_.CreateFAdd(a, builder_.CreateFMul(b, ratioReal)), denominatorReal);
                auto* resultImaginaryA = builder_.CreateFDiv(builder_.CreateFSub(b, builder_.CreateFMul(a, ratioReal)), denominatorReal);
                auto* realDominantEnd = builder_.GetInsertBlock();

                builder_.CreateBr(mergeBlock);

                builder_.SetInsertPoint(imaginaryDominantBlock);

                auto* ratioImaginary = builder_.CreateFDiv(c, d);
                auto* denominatorImaginary = builder_.CreateFAdd(d, builder_.CreateFMul(c, ratioImaginary));
                auto* resultRealB = builder_.CreateFDiv(builder_.CreateFAdd(builder_.CreateFMul(a, ratioImaginary), b), denominatorImaginary);

                auto* resultImaginaryB = builder_.CreateFDiv(builder_.CreateFSub(builder_.CreateFMul(b, ratioImaginary), a), denominatorImaginary);

                auto* imaginaryDominantEnd = builder_.GetInsertBlock();
                builder_.CreateBr(mergeBlock);
                builder_.SetInsertPoint(mergeBlock);

                auto* resultReal = builder_.CreatePHI(builder_.getDoubleTy(), 2, "complex.divide.result.real");

                resultReal->addIncoming(resultRealA, realDominantEnd);
                resultReal->addIncoming(resultRealB, imaginaryDominantEnd);

                auto* resultImaginary = builder_.CreatePHI(builder_.getDoubleTy(), 2, "complex.divide.result.imaginary");

                resultImaginary->addIncoming(resultImaginaryA, realDominantEnd);
                resultImaginary->addIncoming(resultImaginaryB, imaginaryDominantEnd);

                return createComplex(resultReal, resultImaginary);
            }

            if (op == "^") {
                const auto zero = llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0);
                const auto one = llvm::ConstantFP::get(builder_.getDoubleTy(), 1.0);

                auto* realBaseIsZero = builder_.CreateFCmpOEQ(a, zero);
                auto* imaginaryBaseIsZero = builder_.CreateFCmpOEQ(b, zero);

                auto* baseIsZero = builder_.CreateAnd(realBaseIsZero, imaginaryBaseIsZero, "complex.power.zero.base");

                auto* realExponentIsZero = builder_.CreateFCmpOEQ(c, zero);
                auto* imaginaryExponentIsZero = builder_.CreateFCmpOEQ(d, zero);

                auto* exponentIsZero = builder_.CreateAnd(realExponentIsZero, imaginaryExponentIsZero, "complex.power.zero.exponent");

                auto* imaginaryExponentIsNonzero = builder_.CreateFCmpONE(d, zero);
                auto* realExponentIsNegative = builder_.CreateFCmpOLT(c, zero);
                auto* invalidZeroExponent = builder_.CreateOr(imaginaryExponentIsNonzero, realExponentIsNegative);
                auto* invalidZeroPower = builder_.CreateAnd(baseIsZero, invalidZeroExponent);
                emitAbortIf(invalidZeroPower, expression->location, "Zero cannot be raised to a negative or non-real power");

                auto* realExponentUnordered = builder_.CreateFCmpUNO(c, c);
                auto* imaginaryExponentUnordered = builder_.CreateFCmpUNO(d, d);

                auto* invalidExponent = builder_.CreateOr(realExponentUnordered, imaginaryExponentUnordered);
                auto* maximum = llvm::ConstantFP::get(builder_.getDoubleTy(), std::numeric_limits<double>::max());

                auto* absoluteRealExponent = builder_.CreateCall(mathFunctions_.at("fabs"), {c});
                auto* absoluteImaginaryExponent = builder_.CreateCall(mathFunctions_.at("fabs"), {d});

                auto* realExponentTooLarge = builder_.CreateFCmpOGT(absoluteRealExponent, maximum);
                auto* imgainaryExponentTooLarge = builder_.CreateFCmpOGT(absoluteImaginaryExponent, maximum);
                invalidExponent = builder_.CreateOr(invalidExponent, builder_.CreateOr(realExponentTooLarge, imgainaryExponentTooLarge));

                emitAbortIf(invalidExponent, expression->location, "Complex exponentiation requires finite exponent components");

                auto* magnitude = builder_.CreateCall(mathFunctions_.at("hypot"), {a, b}, "complex.power.magnitude");
                auto* argument = builder_.CreateCall(mathFunctions_.at("atan2"), {b, a}, "complex.power.argument");

                auto* logMagnitude = builder_.CreateCall(mathFunctions_.at("log"), {magnitude}, "complex.power.log.magnitude");

                auto* realProduct = builder_.CreateFSub(builder_.CreateFMul(c, logMagnitude), builder_.CreateFMul(d, argument), "complex.power.log.real");
                auto* imaginaryProduct = builder_.CreateFAdd(builder_.CreateFMul(c, argument), builder_.CreateFMul(d, logMagnitude), "complex.power.log.imaginary");

                auto* exponentialMagnitude = builder_.CreateCall(mathFunctions_.at("exp"), {realProduct}, "complex.power.exponential");
                auto* cosine = builder_.CreateCall(mathFunctions_.at("cos"), {imaginaryProduct}, "complex.power.cosine");
                auto* sine = builder_.CreateCall(mathFunctions_.at("sin"), {imaginaryProduct}, "complex.power.sine");

                auto* resultReal = builder_.CreateFMul(exponentialMagnitude, cosine, "complex.power.result.real");
                auto* resultImaginary = builder_.CreateFMul(exponentialMagnitude, sine, "complex.power.result.imaginary");

                auto* zeroBaseNonzeroExponent = builder_.CreateAnd(baseIsZero, builder_.CreateNot(exponentIsZero));
                resultReal = builder_.CreateSelect(zeroBaseNonzeroExponent, zero, resultReal);
                resultImaginary = builder_.CreateSelect(zeroBaseNonzeroExponent, zero, resultImaginary);

                resultReal = builder_.CreateSelect(exponentIsZero, one, resultReal);
                resultImaginary = builder_.CreateSelect(exponentIsZero, zero, resultImaginary);

                auto* realResultUnordered = builder_.CreateFCmpUNO(resultReal, resultReal);
                auto* imaginaryResultUnordered = builder_.CreateFCmpUNO(resultImaginary, resultImaginary);

                emitAbortIf(builder_.CreateOr(realResultUnordered, imaginaryResultUnordered), expression->location, "Complex exponentiation produced an undefined result");

                auto* absoluteResultReal = builder_.CreateCall(mathFunctions_.at("fabs"), {resultReal});
                auto* absoluteResultImaginary = builder_.CreateCall(mathFunctions_.at("fabs"), {resultImaginary});

                auto* realOverflow = builder_.CreateFCmpOGT(absoluteResultReal, maximum);
                auto* imaginaryOverflow = builder_.CreateFCmpOGT(absoluteResultImaginary, maximum);
                emitAbortIf(builder_.CreateOr(realOverflow, imaginaryOverflow), expression->location, "Complex exponentiation overflowed");
                return createComplex(resultReal, resultImaginary);
            }
        }

        if (effectiveType == ValueType::Rational) {
            if (op == "^") {
                auto* left = emitExpression(leftExpr, ValueType::Rational);
                auto* right = emitExpression(rightExpr, ValueType::Integer);

                if (!left || !right)
                    return nullptr;

                return builder_.CreateCall(
                    rationalPowerFunction(),
                    {left, right},
                    "rational.power"
                );
            }

            auto* left = emitExpression(leftExpr, ValueType::Rational);
            auto* right = emitExpression(rightExpr, ValueType::Rational);

            if (!left || !right)
                return nullptr;

            return emitRationalOperation(
                op,
                left,
                right,
                expression->location
            );
        }

        if (effectiveType == ValueType::Real) {
            auto* left = emitRealExpression(leftExpr);

            llvm::Value* rationalExponent = nullptr;
            llvm::Value* right = nullptr;

            if (op == "^" && rightExpr->type == ValueType::Rational) {
                rationalExponent = emitExpression(rightExpr, ValueType::Rational);
                if (!rationalExponent) return nullptr;
                right = coerce(rationalExponent, ValueType::Rational, ValueType::Real, expression->location);
            } else {
                right = emitRealExpression(rightExpr);
            }

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
                const auto zero = llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0);
                auto* baseIsZero = builder_.CreateFCmpOEQ(left, zero);
                llvm::Value* result = nullptr;
                
                if (rationalExponent) {
                    auto* exponentNumerator = builder_.CreateExtractValue(rationalExponent, {0});
                    auto* exponentDenominator = builder_.CreateExtractValue(rationalExponent, {1});
                    auto* exponentIsNegative = builder_.CreateICmpSLT(exponentNumerator, builder_.getInt64(0));
                    emitAbortIf(builder_.CreateAnd(baseIsZero, exponentIsNegative), expression->location, "Zero cannot be raised to a negative power");

                    auto* baseIsNegative = builder_.CreateFCmpOLT(left, zero);
                    auto* denominatorIsEven = builder_.CreateICmpEQ(builder_.CreateAnd(exponentDenominator, builder_.getInt64(1)), builder_.getInt64(0));
                    emitAbortIf(builder_.CreateAnd(baseIsNegative, denominatorIsEven), expression->location, "Negative base with an even-denominator Rational exponent has no real result");

                    auto* absoluteBase = builder_.CreateSelect(baseIsNegative, builder_.CreateFNeg(left), left, "power.absolute.base");
                    auto* magnitude = builder_.CreateCall(pow_, {absoluteBase, right}, "rational.power.magnitude");
                    auto* exponentNumeratorIsOdd = builder_.CreateICmpNE(builder_.CreateAnd(exponentNumerator, builder_.getInt64(1)), builder_.getInt64(0));
                    auto* negateResult = builder_.CreateAnd(baseIsNegative, exponentNumeratorIsOdd);

                    result = builder_.CreateSelect(negateResult, builder_.CreateFNeg(magnitude), magnitude, "rational.power.result");
                } else {
                    auto* exponentIsNegative = builder_.CreateFCmpOLT(right, zero);
                    emitAbortIf(builder_.CreateAnd(baseIsZero, exponentIsNegative), expression->location, "Zero cannot be raised to a negative power");
                    result = builder_.CreateCall(pow_, {left, right}, "real.power");
                }
                auto* unordered = builder_.CreateFCmpUNO(result, result);
                emitAbortIf(unordered, expression->location, "Real exponential is outside the Real domain");
                auto* absoluteResult = builder_.CreateCall(mathFunctions_.at("fabs"), {result}, "power.absolute.result");
                auto* maximum = llvm::ConstantFP::get(builder_.getDoubleTy(), std::numeric_limits<double>::max());

                emitAbortIf(builder_.CreateFCmpOGT(absoluteResult, maximum), expression->location, "Real exponentiation overflowed");

                return result;
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

    llvm::Value* emitRationalOperation(const std::string& op, llvm::Value* left, llvm::Value* right, const SourceLocation& location) {
        auto* leftNumerator64 = builder_.CreateExtractValue(left, {0});
        auto* leftDenominator64 = builder_.CreateExtractValue(left, {1});
        
        auto* rightNumerator64 = builder_.CreateExtractValue(right, {0});
        auto* rightDenominator64 = builder_.CreateExtractValue(right, {1});

        auto* integerType = llvm::IntegerType::get(context_, 128);

        auto* leftNumerator = builder_.CreateSExt(leftNumerator64, integerType);
        auto* leftDenominator = builder_.CreateSExt(leftDenominator64, integerType);
        auto* rightNumerator = builder_.CreateSExt(rightNumerator64, integerType);
        auto* rightDenominator = builder_.CreateSExt(rightDenominator64, integerType);

        llvm::Value* numerator = nullptr;
        llvm::Value* denominator = nullptr;

        if (op == "+") {
            numerator = builder_.CreateAdd(builder_.CreateMul(leftNumerator, rightDenominator), 
                                           builder_.CreateMul(rightNumerator, leftDenominator), "rational.add.numerator");
            denominator = builder_.CreateMul(leftDenominator, rightDenominator, "rational.add.denominator");
        } else if (op == "-") {
            numerator = builder_.CreateSub(builder_.CreateMul(leftNumerator, rightDenominator),
                                           builder_.CreateMul(rightNumerator, leftDenominator), "rational.subtract.numerator");
            denominator = builder_.CreateMul(leftDenominator, rightDenominator, "rational.subtract.denominator");
        } else if (op == "*") {
            numerator = builder_.CreateMul(leftNumerator, rightNumerator, "rational.multiply.numerator");
            denominator = builder_.CreateMul(leftDenominator, rightDenominator, "rational.multiply.denominator");
        } else if (op == "/") {
            auto* divisionByZero = builder_.CreateICmpEQ(rightNumerator64, builder_.getInt64(0));
            emitAbortIf(divisionByZero, location, "Rational division by zero");
            numerator = builder_.CreateMul(leftNumerator, rightDenominator, "rational.divide.numerator");
            denominator = builder_.CreateMul(leftDenominator, rightNumerator, "rational.divide.denominator");
        } else {
            error(location, "FeatureError", "Unsupported native Rational arithmetic operation '" + op + "'");
            return nullptr;
        }

        return emitRationalNormalize(numerator, denominator, location);
    }

    llvm::Function* rationalMultiplyFunction() {
        if (rationalMultiply_) 
            return rationalMultiply_;
        
        auto* rationalType = rationalLLVMType();
        auto* integerType = llvm::IntegerType::get(context_, 128);
        auto* functionType = llvm::FunctionType::get(rationalType, {rationalType, rationalType}, false);
        rationalMultiply_ = llvm::Function::Create(functionType, llvm::Function::InternalLinkage, "cmath.rational.multiply", module_.get());
        auto argument = rationalMultiply_->arg_begin();

        llvm::Value* left = &*argument++;
        llvm::Value* right = &*argument;

        auto* entry = llvm::BasicBlock::Create(context_, "entry", rationalMultiply_);
        auto* failure = llvm::BasicBlock::Create(context_, "overflow", rationalMultiply_);
        auto* success = llvm::BasicBlock::Create(context_, "success", rationalMultiply_);

        llvm::IRBuilder<> helperBuilder(context_);
        helperBuilder.SetInsertPoint(entry);

        auto* aNumerator = helperBuilder.CreateSExt(helperBuilder.CreateExtractValue(left, {0}), integerType);
        auto* aDenominator = helperBuilder.CreateSExt(helperBuilder.CreateExtractValue(left, {1}), integerType);
        auto* bNumerator = helperBuilder.CreateSExt(helperBuilder.CreateExtractValue(right, {0}), integerType);
        auto* bDenominator = helperBuilder.CreateSExt(helperBuilder.CreateExtractValue(right, {1}), integerType);

        auto* zero = llvm::ConstantInt::get(integerType, 0);
        auto* aNegative = helperBuilder.CreateICmpSLT(aNumerator, zero);
        auto* bNegative = helperBuilder.CreateICmpSLT(bNumerator, zero);
        auto* absANumerator = helperBuilder.CreateSelect(aNegative, helperBuilder.CreateNeg(aNumerator), aNumerator);
        auto* absBNumerator = helperBuilder.CreateSelect(bNegative, helperBuilder.CreateNeg(bNumerator), bNumerator);

        auto* gcd1 = helperBuilder.CreateCall(rationalGcdFunction(), {absANumerator, bDenominator}, "rational.multiply.gcdi");
        auto* gcd2 = helperBuilder.CreateCall(rationalGcdFunction(), {absBNumerator, aDenominator}, "rational.multiply.gcd2");
    
        auto divideExact = [&](llvm::Value* dividend, llvm::Value* divisor) -> llvm::Value* {
            auto* division = helperBuilder.CreateCall(rationalDivModFunction(), {dividend, divisor});
            return helperBuilder.CreateExtractValue(division, {0});
        };

        auto* reducedANumerator = divideExact(absANumerator, gcd1);
        auto* reducedBDenominator = divideExact(bDenominator, gcd1);

        auto* reducedBNumerator = divideExact(absBNumerator, gcd2);
        auto* reducedADenominator = divideExact(aDenominator, gcd2);
        auto* numeratorMagnitude = helperBuilder.CreateMul(reducedANumerator, reducedBNumerator, "rational.multiply.numerator");

        auto* denominator = helperBuilder.CreateMul(reducedADenominator, reducedBDenominator, "rational.multiply.denominator");
        auto* negative = helperBuilder.CreateXor(aNegative, bNegative);

        auto* numerator = helperBuilder.CreateSelect(negative, helperBuilder.CreateNeg(numeratorMagnitude), numeratorMagnitude);

        const auto minimum = llvm::ConstantInt::getSigned(integerType, std::numeric_limits<std::int64_t>::min());
        const auto maximum = llvm::ConstantInt::getSigned(integerType, std::numeric_limits<std::int64_t>::max());

        auto* numeratorTooSmall = helperBuilder.CreateICmpSLT(numerator, minimum);
        auto* numeratorTooLarge = helperBuilder.CreateICmpSGT(numerator, maximum);

        auto* denominatorTooLarge = helperBuilder.CreateICmpUGT(denominator, maximum);
        auto* invalid = helperBuilder.CreateOr(numeratorTooSmall, helperBuilder.CreateOr(numeratorTooLarge, denominatorTooLarge));
        helperBuilder.CreateCondBr(invalid, failure, success);
        helperBuilder.SetInsertPoint(failure);
        helperBuilder.CreateCall(printf_,
        {
            helperBuilder.CreateGlobalString(
                "CMath runtime error: Rational multiplication overflow\n"
            )
        });

        helperBuilder.CreateCall(fflush_, {llvm::ConstantPointerNull::get(helperBuilder.getPtrTy())});

        helperBuilder.CreateCall(abort_);
        helperBuilder.CreateUnreachable();

        helperBuilder.SetInsertPoint(success);
        auto* numerator64 = helperBuilder.CreateTrunc(numerator, helperBuilder.getInt64Ty());
        auto* denominator64 = helperBuilder.CreateTrunc(denominator, helperBuilder.getInt64Ty());

        auto* undefResult = llvm::UndefValue::get(rationalType);
        auto* result = helperBuilder.CreateInsertValue(undefResult, numerator64, {0});
        result = helperBuilder.CreateInsertValue(result, denominator64, {1});
        helperBuilder.CreateRet(result);
        return rationalMultiply_;
    }

    llvm::Function* rationalPowerFunction() {
        if (rationalPower_) 
            return rationalPower_;

        auto* multiplyFunction = rationalMultiplyFunction();

        auto* rationalType = rationalLLVMType();
        auto* integerType = builder_.getInt64Ty();

        auto* functionType = llvm::FunctionType::get(rationalType, {rationalType, integerType}, false);
        rationalPower_ = llvm::Function::Create(functionType, llvm::Function::InternalLinkage, "cmath.rational.power", module_.get());

        auto argument = rationalPower_->arg_begin();

        llvm::Value* initialBase = &*argument++;
        llvm::Value* initialExponent = &*argument;

        auto* entry = llvm::BasicBlock::Create(context_, "entry", rationalPower_);
        auto* failure = llvm::BasicBlock::Create(context_, "invalid", rationalPower_);
        auto* prepare = llvm::BasicBlock::Create(context_, "prepare", rationalPower_);
        auto* loop = llvm::BasicBlock::Create(context_, "loop", rationalPower_);
        auto* body = llvm::BasicBlock::Create(context_, "body", rationalPower_);
        auto* multiply = llvm::BasicBlock::Create(context_, "multiply", rationalPower_);
        auto* skipMultiply = llvm::BasicBlock::Create(context_, "skip.multiply", rationalPower_);
        auto* afterMultiply = llvm::BasicBlock::Create(context_, "after.multiply", rationalPower_);
        auto* square = llvm::BasicBlock::Create(context_, "square", rationalPower_);
        auto* skipSquare = llvm::BasicBlock::Create(context_, "skip.square", rationalPower_);
        auto* iterationEnd = llvm::BasicBlock::Create(context_, "iteration.end", rationalPower_);
        auto* exit = llvm::BasicBlock::Create(context_, "exit", rationalPower_);
        
        llvm::IRBuilder<> helperBuilder(context_);

        auto* zero = helperBuilder.getInt64(0);
        auto* one = helperBuilder.getInt64(1);

        helperBuilder.SetInsertPoint(entry);

        auto* numerator = helperBuilder.CreateExtractValue(initialBase, {0});
        auto* denominator = helperBuilder.CreateExtractValue(initialBase, {1});

        auto* negativeExponent = helperBuilder.CreateICmpSLT(initialExponent, zero);
        auto* zeroBase = helperBuilder.CreateICmpEQ(numerator, zero);

        auto* minimumNumerator = llvm::ConstantInt::getSigned(helperBuilder.getInt64Ty(), std::numeric_limits<std::int64_t>::min());
        auto* cannotInvert = helperBuilder.CreateICmpEQ(numerator, minimumNumerator);

        auto* invalidZeroPower = helperBuilder.CreateAnd(negativeExponent, zeroBase);
        auto* invalidReciprocal = helperBuilder.CreateAnd(negativeExponent, cannotInvert);

        auto* invalid = helperBuilder.CreateOr(invalidZeroPower, invalidReciprocal);
        helperBuilder.CreateCondBr(invalid, failure, prepare);

        helperBuilder.SetInsertPoint(failure);
        helperBuilder.CreateCall(
            printf_,
            {
                helperBuilder.CreateGlobalString("CMath runtime error: Invalid or overflowing negative Rational power\n")
            }
        );

        helperBuilder.CreateCall(fflush_, {llvm::ConstantPointerNull::get(helperBuilder.getPtrTy())});

        helperBuilder.CreateCall(abort_);
        helperBuilder.CreateUnreachable();

        helperBuilder.SetInsertPoint(prepare);
        auto* numeratorNegative = helperBuilder.CreateICmpSLT(numerator, zero);
        auto* absoluteNumerator = helperBuilder.CreateSelect(numeratorNegative, helperBuilder.CreateNeg(numerator), numerator);
        auto* reciprocalNumerator = helperBuilder.CreateSelect(numeratorNegative, helperBuilder.CreateNeg(denominator), denominator);
        auto* baseNumerator = helperBuilder.CreateSelect(negativeExponent, reciprocalNumerator, numerator);
        auto* baseDenominator = helperBuilder.CreateSelect(negativeExponent, absoluteNumerator, denominator);
        auto* undefBase = llvm::UndefValue::get(rationalType);
        auto* base = helperBuilder.CreateInsertValue(undefBase, baseNumerator, {0});
        base = helperBuilder.CreateInsertValue(base, baseDenominator, {1});

        auto* negatedExponent = helperBuilder.CreateNeg(initialExponent);
        auto* exponentMagnitude = helperBuilder.CreateSelect(negativeExponent, negatedExponent, initialExponent);
        auto* oneRational = llvm::ConstantStruct::get(rationalType, {helperBuilder.getInt64(1), helperBuilder.getInt64(1)});

        helperBuilder.CreateBr(loop);
        helperBuilder.SetInsertPoint(loop);

        auto* basePhi = helperBuilder.CreatePHI(rationalType, 2, "power.base");
        auto* resultPhi = helperBuilder.CreatePHI(rationalType, 2, "power.result");
        auto* exponentPhi = helperBuilder.CreatePHI(integerType, 2, "power.exponent");

        basePhi->addIncoming(base, prepare);
        resultPhi->addIncoming(oneRational, prepare);
        exponentPhi->addIncoming(exponentMagnitude, prepare);

        auto* finished = helperBuilder.CreateICmpEQ(exponentPhi, zero);
        helperBuilder.CreateCondBr(finished, exit, body);

        helperBuilder.SetInsertPoint(body);

        auto* exponentIsOdd = helperBuilder.CreateICmpNE(helperBuilder.CreateAnd(exponentPhi, one), zero);

        helperBuilder.CreateCondBr(exponentIsOdd, multiply, skipMultiply);
        helperBuilder.SetInsertPoint(multiply);

        auto* product = helperBuilder.CreateCall(multiplyFunction, {resultPhi, basePhi}, "power.product");
        helperBuilder.CreateBr(afterMultiply);
        helperBuilder.SetInsertPoint(skipMultiply);
        helperBuilder.CreateBr(afterMultiply);
        helperBuilder.SetInsertPoint(afterMultiply);

        auto* nextResult = helperBuilder.CreatePHI(rationalType, 2, "power.next.result");
        nextResult->addIncoming(product, multiply);
        nextResult->addIncoming(resultPhi, skipMultiply);
        
        auto* nextExponent = helperBuilder.CreateLShr(exponentPhi, one);
        auto* needsSquare = helperBuilder.CreateICmpNE(nextExponent, zero);
        helperBuilder.CreateCondBr(needsSquare, square, skipSquare);

        helperBuilder.SetInsertPoint(square);
        auto* squared = helperBuilder.CreateCall(multiplyFunction, {basePhi, basePhi}, "power.square");

        helperBuilder.CreateBr(iterationEnd);
        helperBuilder.SetInsertPoint(skipSquare);
        helperBuilder.CreateBr(iterationEnd);
        helperBuilder.SetInsertPoint(iterationEnd);

        auto* nextBase = helperBuilder.CreatePHI(rationalType, 2, "power.next.base");
        nextBase->addIncoming(squared, square);
        nextBase->addIncoming(basePhi, skipSquare);

        helperBuilder.CreateBr(loop);

        basePhi->addIncoming(nextBase, iterationEnd);
        resultPhi->addIncoming(nextResult, iterationEnd);
        exponentPhi->addIncoming(nextExponent, iterationEnd);

        helperBuilder.SetInsertPoint(exit);
        helperBuilder.CreateRet(resultPhi);

        return rationalPower_;
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
        
        builder_.CreateCall(printf_, {makeString(diagnostic, "cmath.runtime.message")});

        builder_.CreateCall(fflush_, {llvm::ConstantPointerNull::get(builder_.getPtrTy())});

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
            } else if (argumentExpr->type == ValueType::Rational) {
                auto* value = emitExpression(argumentExpr);

                if (!value) return nullptr;

                auto* numerator = builder_.CreateExtractValue(value, {0}, "rational.print.numerator");
                auto* denominator = builder_.CreateExtractValue(value, {1}, "rational.print.denominator");

                auto* function = builder_.GetInsertBlock()->getParent();
                auto* integerBlock = llvm::BasicBlock::Create(context_, "rational.print.integer", function);
                auto* fractionBlock = llvm::BasicBlock::Create(context_, "rational.print.fraction", function);
                auto* mergeBlock = llvm::BasicBlock::Create(context_, "rational.print.end", function);

                auto* isInteger = builder_.CreateICmpEQ(denominator, builder_.getInt64(1));
                builder_.CreateCondBr(isInteger, integerBlock, fractionBlock);
                builder_.SetInsertPoint(integerBlock);
                builder_.CreateCall(printf_, {
                    makeString("%lld", "cmath.fmt.rational.integer"),
                    numerator
                });
                
                builder_.CreateBr(mergeBlock);
                builder_.SetInsertPoint(fractionBlock);
                builder_.CreateCall(printf_, 
                    {
                        makeString("%lld/%lld", "cmath.fmt.rational.fraction"),
                        numerator,
                        denominator
                    });
                
                builder_.CreateBr(mergeBlock);
                builder_.SetInsertPoint(mergeBlock);
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
            } else if (argumentExpr->type == ValueType::Complex) {
                auto* value = emitExpression(argumentExpr);
                if (!value) return nullptr;

                auto* realPart = builder_.CreateExtractValue(value, {0}, "complex.print.real");
                auto* imaginaryPart = builder_.CreateExtractValue(value, {1}, "complex.print.imaginary");

                builder_.CreateCall(printf_, {
                    makeString("%.15g%+.15gi", "cmath.fmt.complex"), realPart, imaginaryPart
                });
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
        builder_.CreateCall(
            fflush_,
            {llvm::ConstantPointerNull::get(builder_.getPtrTy())}
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
                    if (node.lambdaFunction || node.typeInfo.isFunction()) {
                        error(statement.location, "FeatureError", "Native compilation of lambda values and closures is not supported yet");
                        return false;
                    }

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
    llvm::FunctionCallee fflush_;
    llvm::FunctionCallee strcmp_;
    llvm::FunctionCallee pow_;

    llvm::StructType* rationalType_ = nullptr;
    llvm::StructType* complexType_ = nullptr;
    llvm::StructType* rationalDivModType_ = nullptr;
    llvm::Function* rationalGcd_ = nullptr;
    llvm::Function* rationalDivMod_ = nullptr;

    llvm::Function* rationalMultiply_ = nullptr;
    llvm::Function* rationalPower_ = nullptr;

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
