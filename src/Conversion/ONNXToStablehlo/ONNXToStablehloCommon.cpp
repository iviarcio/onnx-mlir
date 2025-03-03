/*
 * SPDX-License-Identifier: Apache-2.0
 */

//====-- ONNXToStablehloCommon.cpp - ONNX dialects to Stablehlo lowering--===//
//
// Copyright 2022-2024
//
// =============================================================================
//
// This file contains common code shared by the functions performing the
// lowering to the Stablehlo dialect.
//
//===----------------------------------------------------------------------===//

#include "src/Conversion/ONNXToStablehlo/ONNXToStablehloCommon.hpp"
#include "src/Dialect/ONNX/ONNXOps/OpHelper.hpp"
#include "src/Dialect/ONNX/OnnxElementsAttrBuilder.hpp"

#include "stablehlo/dialect/BroadcastUtils.h"

using namespace mlir;

namespace onnx_mlir {

Value getShapedZero(
    Location loc, ConversionPatternRewriter &rewriter, Value &inp) {
  ShapedType inpType = inp.getType().cast<ShapedType>();
  Value broadcastedZero;
  if (inpType.hasStaticShape())
    broadcastedZero = rewriter.create<stablehlo::ConstantOp>(
        loc, rewriter.getZeroAttr(inpType));
  else {
    Type elemType = inpType.getElementType();
    Value zero = rewriter.create<stablehlo::ConstantOp>(
        loc, rewriter.getZeroAttr(elemType));
    Value shape = rewriter.create<shape::ShapeOfOp>(loc, inp);
    broadcastedZero = rewriter.create<stablehlo::DynamicBroadcastInDimOp>(
        loc, inpType, zero, shape, rewriter.getI64TensorAttr({}));
  }
  return broadcastedZero;
}

llvm::SmallVector<Value, 4> getBroadcastedOperands(Operation *op,
    ConversionPatternRewriter &rewriter, Location loc, int64_t outputRank) {
  llvm::SmallVector<Value, 4> broadcastedOperands;
  Type outputType = *op->result_type_begin();
  assert(outputType.isa<ShapedType>() && "output type is not shaped");
  ShapedType outputShapedType = outputType.cast<ShapedType>();
  Value resultExtents =
      mlir::hlo::computeNaryElementwiseBroadcastingResultExtents(
          loc, op->getOperands(), rewriter);
  for (Value operand : op->getOperands()) {
    RankedTensorType operandType =
        operand.getType().dyn_cast<RankedTensorType>();
    assert(operandType != nullptr && "operand type is not ranked");
    SmallVector<int64_t, 4> broadcastDimensions = llvm::to_vector<4>(
        llvm::seq<int64_t>(outputRank - operandType.getRank(), outputRank));
    Type elementType =
        operand.getType().dyn_cast<ShapedType>().getElementType();
    RankedTensorType broadcastedOutputType =
        RankedTensorType::get(outputShapedType.getShape(), elementType);
    Value broadcast = rewriter.create<stablehlo::DynamicBroadcastInDimOp>(loc,
        broadcastedOutputType, operand, resultExtents,
        rewriter.getI64TensorAttr(broadcastDimensions));
    broadcastedOperands.push_back(broadcast);
  }
  return broadcastedOperands;
}

llvm::SmallVector<Value, 4> getBroadcastedOperands(
    llvm::SmallVector<Value, 4> &operands, Type outputType,
    ConversionPatternRewriter &rewriter, Location loc, int64_t outputRank) {
  llvm::SmallVector<Value, 4> broadcastedOperands;
  assert(outputType.isa<ShapedType>() && "output type is not shaped");
  ShapedType outputShapedType = outputType.cast<ShapedType>();
  Value resultExtents =
      mlir::hlo::computeNaryElementwiseBroadcastingResultExtents(
          loc, operands, rewriter);
  for (Value operand : operands) {
    RankedTensorType operandType =
        operand.getType().dyn_cast<RankedTensorType>();
    assert(operandType != nullptr && "operand type is not ranked");
    SmallVector<int64_t, 4> broadcastDimensions = llvm::to_vector<4>(
        llvm::seq<int64_t>(outputRank - operandType.getRank(), outputRank));
    Type elementType =
        operands[0].getType().dyn_cast<ShapedType>().getElementType();
    RankedTensorType broadcastedOutputType =
        RankedTensorType::get(outputShapedType.getShape(), elementType);
    Value broadcast = rewriter.create<stablehlo::DynamicBroadcastInDimOp>(loc,
        broadcastedOutputType, operand, resultExtents,
        rewriter.getI64TensorAttr(broadcastDimensions));
    broadcastedOperands.push_back(broadcast);
  }
  return broadcastedOperands;
}

ElementsAttr getElementAttributeFromConstValue(Value value) {
  auto definingOp = value.getDefiningOp();
  if (auto constantOp = dyn_cast_or_null<stablehlo::ConstantOp>(definingOp)) {
    return constantOp.getValue().dyn_cast<ElementsAttr>();
  } else if (auto constantOp =
                 dyn_cast_or_null<mlir::ONNXConstantOp>(definingOp)) {
    if (constantOp.getValue().has_value())
      return constantOp.getValueAttr().dyn_cast<ElementsAttr>();
  }
  return nullptr;
}

DenseIntElementsAttr GetI64ElementsAttr(
    ArrayRef<int64_t> values, Builder *builder) {
  RankedTensorType ty = RankedTensorType::get(
      {static_cast<int64_t>(values.size())}, builder->getIntegerType(64));
  return DenseIntElementsAttr::get(ty, values);
}

namespace {
// Returns the DenseElementsAttr of input if it's a stablehlo constant or
// onnx.Constant. Otherwise returns a nullptr attribute.
DenseElementsAttr getDenseElementAttrFromConstValue(mlir::Value value) {
  Operation *definingOp = value.getDefiningOp();
  if (auto globalOp = dyn_cast_or_null<stablehlo::ConstantOp>(definingOp)) {
    return globalOp.getValueAttr().dyn_cast<DenseElementsAttr>();
  } else if (auto constOp =
                 dyn_cast_or_null<mlir::ONNXConstantOp>(definingOp)) {
    if (constOp.getValue().has_value())
      return constOp.getValueAttr().dyn_cast<DenseElementsAttr>();
  }
  return nullptr;
}
} // namespace

/// Emit an ONNXSqueezeOp. If the input is constant, do const propagation,
/// and return a constant.
Value foldOrEmitONNXSqueezeOpStablehlo(ConversionPatternRewriter &rewriter,
    Location loc, Type resultType, Value input, int64_t axis) {
  MultiDialectBuilder<OnnxBuilder> create(rewriter, loc);
  return create.onnx.foldOrEmitONNXSqueezeOp(rewriter, loc, resultType, input,
      axis, getDenseElementAttrFromConstValue);
}

/// Emit an ONNXUnsqueezeOp. If the input is constant, do const
/// propagation, and return a constant.
Value foldOrEmitONNXUnsqueezeOpStablehlo(ConversionPatternRewriter &rewriter,
    Location loc, Type resultType, Value input, int64_t axis) {
  MultiDialectBuilder<OnnxBuilder> create(rewriter, loc);
  return create.onnx.foldOrEmitONNXUnsqueezeOp(rewriter, loc, resultType, input,
      axis, getDenseElementAttrFromConstValue);
}

/// Emit an ONNXSplitOp. If the input is constant, do const propagation, and
/// return constants.
/// Only support evenly splitting.
std::vector<Value> foldOrEmitONNXSplitOpStablehlo(
    ConversionPatternRewriter &rewriter, Location loc,
    ArrayRef<Type> resultTypes, Value input, int64_t axis) {
  MultiDialectBuilder<OnnxBuilder> create(rewriter, loc);
  return create.onnx.foldOrEmitONNXSplitOp(rewriter, loc, resultTypes, input,
      axis, getDenseElementAttrFromConstValue);
}

/// Emit an ONNXTransposeOp. If the input is constant, do const propagation,
/// and return a constant.
Value foldOrEmitONNXTransposeOpStablehlo(ConversionPatternRewriter &rewriter,
    Location loc, Type resultType, Value input, ArrayAttr permAttr) {
  MultiDialectBuilder<OnnxBuilder> create(rewriter, loc);
  return create.onnx.foldOrEmitONNXTransposeOp(rewriter, loc, resultType, input,
      permAttr, getDenseElementAttrFromConstValue);
}

} // namespace onnx_mlir
