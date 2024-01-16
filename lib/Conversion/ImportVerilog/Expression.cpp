//===- Statement.cpp - Slang expression conversion ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ImportVerilogInternals.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/Type.h"
#include "slang/syntax/SyntaxVisitor.h"

using namespace circt;
using namespace ImportVerilog;

namespace {
struct ExprVisitor {
  Context &context;
  Location loc;
  OpBuilder &builder;

  /// Helper function to convert a value to its simple bit vector
  /// representation, if it has one. Otherwise returns null.
  Value convertToSimpleBitVector(Value value) {
    if (!value)
      return {};
    if (auto type = dyn_cast_or_null<moore::UnpackedType>(value.getType())) {
      if (type.isSimpleBitVector())
        return value;
      if (auto sbvt = type.castToSimpleBitVectorOrNull())
        return builder.create<moore::ConversionOp>(
            loc, sbvt.getType(builder.getContext()), value);
    }
    mlir::emitError(loc, "expression of type ")
        << value.getType() << " cannot be cast to a simple bit vector";
    return {};
  }

  /// Helper function to convert a value to its "truthy" boolean value.
  Value convertToBool(Value value) {
    if (!value)
      return {};
    if (auto type = dyn_cast_or_null<moore::IntType>(value.getType()))
      if (type.getBitSize() == 1)
        return value;
    if (auto type = dyn_cast_or_null<moore::UnpackedType>(value.getType()))
      return builder.create<moore::BoolCastOp>(loc, value);
    mlir::emitError(loc, "expression of type ")
        << value.getType() << " cannot be cast to a boolean";
    return {};
  }

  Value visit(const slang::ast::NamedValueExpression &expr) {
    // TODO: This needs something more robust. Slang should have resolved names
    // already. Better use those pointers instead of names.
    return context.varSymbolTable.lookup(expr.getSymbolReference()->name);
  }

  Value visit(const slang::ast::ConversionExpression &expr) {
    auto type = context.convertType(*expr.type);
    auto operand = context.convertExpression(expr.operand());
    if (!operand)
      return {};
    return builder.create<moore::ConversionOp>(loc, type, operand);
  }

  Value visit(const slang::ast::AssignmentExpression &expr) {
    auto lhs = context.convertExpression(expr.left());
    auto rhs = context.convertExpression(expr.right());
    if (!lhs || !rhs)
      return {};

    if (lhs.getType() != rhs.getType())
      rhs = builder.create<moore::ConversionOp>(loc, lhs.getType(), rhs);

    if (expr.isNonBlocking())
      builder.create<moore::PAssignOp>(loc, lhs, rhs);
    else if (expr.syntax->parent->kind ==
             slang::syntax::SyntaxKind::ContinuousAssign)
      builder.create<moore::CAssignOp>(loc, lhs, rhs);
    else if (expr.syntax->parent->kind ==
             slang::syntax::SyntaxKind::ProceduralAssignStatement)
      builder.create<moore::PCAssignOp>(loc, lhs, rhs);
    else
      builder.create<moore::BPAssignOp>(loc, lhs, rhs);
    return lhs;
  }

  template <class ConcreteOp>
  Value createReduction(Value arg, bool invert) {
    arg = convertToSimpleBitVector(arg);
    if (!arg)
      return {};
    Value result = builder.create<ConcreteOp>(loc, arg);
    if (invert)
      result = builder.create<moore::NotOp>(loc, result);
    return result;
  }

  Value createIncrement(Value arg, bool isInc, bool isPost) {
    auto preValue = convertToSimpleBitVector(arg);
    if (!preValue)
      return {};
    auto sbvt =
        cast<moore::UnpackedType>(preValue.getType()).getSimpleBitVector();
    auto one = builder.create<moore::ConstantOp>(loc, preValue.getType(),
                                                 APInt(sbvt.size, 1));
    auto postValue =
        isInc ? builder.create<moore::AddOp>(loc, preValue, one).getResult()
              : builder.create<moore::SubOp>(loc, preValue, one).getResult();
    builder.create<moore::BPAssignOp>(loc, arg, postValue);
    return isPost ? preValue : postValue;
  }

  Value visit(const slang::ast::UnaryExpression &expr) {
    auto arg = context.convertExpression(expr.operand());
    if (!arg)
      return {};

    using slang::ast::UnaryOperator;
    switch (expr.op) {
      // `+a` is simply `a`, but converted to a simple bit vector type since
      // this is technically an arithmetic operation.
    case UnaryOperator::Plus:
      return convertToSimpleBitVector(arg);

    case UnaryOperator::Minus:
      arg = convertToSimpleBitVector(arg);
      if (!arg)
        return {};
      return builder.create<moore::NegOp>(loc, arg);

    case UnaryOperator::BitwiseNot:
      arg = convertToSimpleBitVector(arg);
      if (!arg)
        return {};
      return builder.create<moore::NotOp>(loc, arg);

    case UnaryOperator::BitwiseAnd:
      return createReduction<moore::ReduceAndOp>(arg, false);
    case UnaryOperator::BitwiseOr:
      return createReduction<moore::ReduceOrOp>(arg, false);
    case UnaryOperator::BitwiseXor:
      return createReduction<moore::ReduceXorOp>(arg, false);
    case UnaryOperator::BitwiseNand:
      return createReduction<moore::ReduceAndOp>(arg, true);
    case UnaryOperator::BitwiseNor:
      return createReduction<moore::ReduceOrOp>(arg, true);
    case UnaryOperator::BitwiseXnor:
      return createReduction<moore::ReduceXorOp>(arg, true);

    case UnaryOperator::LogicalNot:
      arg = convertToBool(arg);
      if (!arg)
        return {};
      return builder.create<moore::NotOp>(loc, arg);

    case UnaryOperator::Preincrement:
      return createIncrement(arg, true, false);
    case UnaryOperator::Predecrement:
      return createIncrement(arg, false, false);
    case UnaryOperator::Postincrement:
      return createIncrement(arg, true, true);
    case UnaryOperator::Postdecrement:
      return createIncrement(arg, false, true);
    }

    mlir::emitError(loc, "unsupported unary operator");
    return {};
  }

  template <class ConcreteOp>
  Value createBinary(Value lhs, Value rhs) {
    lhs = convertToSimpleBitVector(lhs);
    rhs = convertToSimpleBitVector(rhs);
    if (!lhs || !rhs)
      return {};
    return builder.create<ConcreteOp>(loc, lhs, rhs);
  }

  Value visit(const slang::ast::BinaryExpression &expr) {
    auto lhs = context.convertExpression(expr.left());
    auto rhs = context.convertExpression(expr.right());
    if (!lhs || !rhs)
      return {};

    using slang::ast::BinaryOperator;
    switch (expr.op) {
    case BinaryOperator::Add:
      return createBinary<moore::AddOp>(lhs, rhs);
    case BinaryOperator::Subtract:
      return createBinary<moore::SubOp>(lhs, rhs);
    case BinaryOperator::Multiply:
      return createBinary<moore::MulOp>(lhs, rhs);
    case BinaryOperator::Divide:
      return createBinary<moore::DivOp>(lhs, rhs);
    case BinaryOperator::Mod:
      return createBinary<moore::ModOp>(lhs, rhs);

    case BinaryOperator::BinaryAnd:
      return createBinary<moore::AndOp>(lhs, rhs);
    case BinaryOperator::BinaryOr:
      return createBinary<moore::OrOp>(lhs, rhs);
    case BinaryOperator::BinaryXor:
      return createBinary<moore::XorOp>(lhs, rhs);
    case BinaryOperator::BinaryXnor: {
      auto result = createBinary<moore::XorOp>(lhs, rhs);
      if (!result)
        return {};
      return builder.create<moore::NotOp>(loc, result);
    }

    case BinaryOperator::Equality:
      return createBinary<moore::EqOp>(lhs, rhs);
    case BinaryOperator::Inequality:
      return createBinary<moore::NeOp>(lhs, rhs);
    case BinaryOperator::CaseEquality:
      return createBinary<moore::CaseEqOp>(lhs, rhs);
    case BinaryOperator::CaseInequality:
      return createBinary<moore::CaseNeOp>(lhs, rhs);
    case BinaryOperator::WildcardEquality:
      return createBinary<moore::WildcardEqOp>(lhs, rhs);
    case BinaryOperator::WildcardInequality:
      return createBinary<moore::WildcardNeOp>(lhs, rhs);

    case BinaryOperator::GreaterThanEqual:
      return createBinary<moore::GeOp>(lhs, rhs);
    case BinaryOperator::GreaterThan:
      return createBinary<moore::GtOp>(lhs, rhs);
    case BinaryOperator::LessThanEqual:
      return createBinary<moore::LeOp>(lhs, rhs);
    case BinaryOperator::LessThan:
      return createBinary<moore::LtOp>(lhs, rhs);

    case BinaryOperator::LogicalAnd:
      return builder.create<moore::LogicalOp>(loc, moore::Logic::LogicalAnd,
                                              lhs, rhs);
    case BinaryOperator::LogicalOr:
      return builder.create<moore::LogicalOp>(loc, moore::Logic::LogicalOr, lhs,
                                              rhs);
    case BinaryOperator::LogicalImplication:
      return builder.create<moore::LogicalOp>(
          loc, moore::Logic::LogicalImplication, lhs, rhs);
    case BinaryOperator::LogicalEquivalence:
      return builder.create<moore::LogicalOp>(
          loc, moore::Logic::LogicalEquivalence, lhs, rhs);
    case BinaryOperator::LogicalShiftLeft:
      return builder.create<moore::ShlOp>(loc, lhs, rhs);
    case BinaryOperator::LogicalShiftRight:
      return builder.create<moore::ShrOp>(loc, lhs, rhs);
    case BinaryOperator::ArithmeticShiftLeft:
      return builder.create<moore::ShlOp>(loc, lhs, rhs, builder.getUnitAttr());
    case BinaryOperator::ArithmeticShiftRight:
      return builder.create<moore::ShrOp>(loc, lhs, rhs, builder.getUnitAttr());
    case BinaryOperator::Power:
      mlir::emitError(loc, "unsupported binary operator: power");
      return {};
    }

    mlir::emitError(loc, "unsupported binary operator");
    return {};
  }

  Value visit(const slang::ast::IntegerLiteral &expr) {
    // TODO: This is wildly unsafe and breaks for anything larger than 32 bits.
    auto value = expr.getValue().as<uint32_t>().value();
    auto type = context.convertType(*expr.type);
    return builder.create<moore::ConstantOp>(loc, type, value);
  }

  Value visit(const slang::ast::ConcatenationExpression &expr) {
    SmallVector<Value> operands;
    for (auto *operand : expr.operands()) {
      auto value = context.convertExpression(*operand);
      if (!value)
        return {};
      operands.push_back(value);
    }
    return builder.create<moore::ConcatOp>(loc, operands);
  }

  /// Emit an error for all other expressions.
  template <typename T>
  Value visit(T &&node) {
    mlir::emitError(loc, "unsupported expression: ")
        << slang::ast::toString(node.kind);
    return {};
  }

  Value visitInvalid(const slang::ast::Expression &expr) {
    mlir::emitError(loc, "invalid expression");
    return {};
  }
};
} // namespace

Value Context::convertExpression(const slang::ast::Expression &expr) {
  auto loc = convertLocation(expr.sourceRange.start());
  return expr.visit(ExprVisitor{*this, loc, builder});
}