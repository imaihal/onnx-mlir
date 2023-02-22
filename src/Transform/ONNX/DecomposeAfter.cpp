/*
 * SPDX-License-Identifier: Apache-2.0
 */

//=== DecomposeAfter.cpp - ONNX High Level Rewriting after shape inference ===//
//
// Copyright 2019-2023 The IBM Research Authors.
//
// =============================================================================
//
// This file implements a set of rewriters to decompose an ONNX operation into
// composition of other ONNX operations after shape inference.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#include "src/Dialect/ONNX/DialectBuilder.hpp"
#include "src/Dialect/ONNX/ONNXOps.hpp"
#include "src/Dialect/ONNX/ONNXOps/OpHelper.hpp"
#include "src/Pass/Passes.hpp"

using namespace mlir;

namespace onnx_mlir {

// ConvTranspose Op
// Create ONNX Transpose op
// TODO: The same function in ONNXToZHighComon.cpp. Commonize them.
Value emitONNXTranspose(
    Location loc, PatternRewriter &rewriter, Value x, ArrayRef<int64_t> perms) {
  onnx_mlir::MultiDialectBuilder<onnx_mlir::OnnxBuilder> create(rewriter, loc);
  ShapedType inputType = x.getType().cast<ShapedType>();
  Type elementType = inputType.getElementType();
  Type transposedType;
  if (inputType.hasRank()) {
    assert((uint64_t)inputType.getRank() == perms.size() &&
           "Permutation array size is different from the input rank");
    ArrayRef<int64_t> inputShape = inputType.getShape();
    SmallVector<int64_t, 4> transposedShape;
    for (uint64_t i = 0; i < perms.size(); ++i)
      transposedShape.emplace_back(inputShape[perms[i]]);
    transposedType = RankedTensorType::get(transposedShape, elementType);
  } else {
    transposedType = UnrankedTensorType::get(elementType);
  }
  Value result =
      create.onnx.transpose(transposedType, x, rewriter.getI64ArrayAttr(perms));
  return result;
}

// Reverse all elements of the first or second dimension of `input`.
Value reverseAllElements(
    PatternRewriter &rewriter, Location loc, Value input, int64_t dimension) {
  onnx_mlir::MultiDialectBuilder<onnx_mlir::OnnxBuilder> create(rewriter, loc);
  ShapedType inputType = input.getType().cast<ShapedType>();
  ArrayRef<int64_t> inputShape = inputType.getShape();
  SmallVector<int64_t, 4> slens;
  assert((dimension == 0 or dimension == 1) &&
         "Reversed diemsnion need to be 0 or 1.");
  // Create `sequence_lengths`, `batch_axis` and `time_axis` to reverse all
  // elements. When reversing the first dim of input(d0 x d1), set `batch_axis`
  // = 1, and `time_axis` = 0 and create [d0, d0,...,d0] as `sequence_lengths`
  // whose the number of elements are d1.
  // Example:
  //   input(d0 x d1) = (4 x 3)) then, `sequence_lenghts` is [4, 4, 4].
  // When reverse the second dim of input(d0 x d1), set `batch_axis` = 0,
  // and `time_axis` = 1 and create [d1, d1,...,d1] as `sequence_lengths`
  // whose the number of elements are d0.
  // Example:
  // input(d0 x d1) = (4 x 3)) then, `sequence_lenghts` is [3, 3, 3, 3].
  int64_t batchAxis = dimension == 0 ? 1 : 0;
  int64_t timeAxis = dimension == 0 ? 0 : 1;
  for (int i = 0; i < inputShape[batchAxis]; ++i)
    slens.emplace_back(inputShape[timeAxis]);
  Value slensVal = create.onnx.constantInt64(slens);
  Type resultType = input.getType().cast<RankedTensorType>();
  Value result = create.onnx.reverseSequence(
      resultType, input, slensVal, batchAxis, timeAxis);
  return result;
}

// Reverse elements in weight tensor of ConvTranspose op. The reversed weight
// tensor are used as weight tensor of Conv op generated by rewriting.
// 1. Transpose weight tensor from NxCxD0xD1xD2x... to D0xD1xD2x ... xNxC to
//    reverse elements by using ReverseSequence op
//    The ReverseSequence op can reverse elements in the first and second
//    dimensions. So, spatial dimensions are moved using Transpose op.
// 2. Reverse The first two dimensions by two ReverseSequence ops
//    Reverse D0 by the first ReverseSequence op, then reverse D1 by the second
//    ReverseSequence op. Reverse D0 and D1 and move them to last
//    (D0xD1xD2xD3x... to D2xD3x...xD0xD1) to reverse D2 and D3. Continue this
//    to reverse all spatial dimensions.
// 3. Reverse the last spatial dimension (Dn) using single ReverseSequence if
//    rank is odd
// 4. Reverse non-spatial dimensions (N and C)
//    Transpose "N x C x D0 x D1 x D2 x ... x Dn" to "C x N x D0 x D1 x D2 x
//    ...x Dn".
Value reverseWeightTensor(
    PatternRewriter &rewriter, Location loc, Value input) {
  onnx_mlir::MultiDialectBuilder<onnx_mlir::OnnxBuilder> create(rewriter, loc);
  ShapedType inputType = input.getType().cast<ShapedType>();
  Type elementType = inputType.getElementType();
  assert(inputType.hasRank() && "Need rank to reverse weight tensor.");
  // 1. Transpose NxCxD0xD1xD2x... to D0xD1xD2x ... xNxC
  int64_t spatialOffset = 2; // for N and C
  int64_t spatialRank = inputType.getRank() - spatialOffset;
  SmallVector<int64_t, 4> permsval;
  for (int i = 0; i < spatialRank; ++i)
    permsval.emplace_back(spatialOffset + i);
  for (int i = 0; i < spatialOffset; ++i)
    permsval.emplace_back(i);
  ArrayRef<int64_t> perms(permsval);
  Value transposedInput = emitONNXTranspose(loc, rewriter, input, perms);
  // 2. Reverse the first and second dimensions
  for (int i = 0; i < spatialRank / 2; i += 2) {
    Value reverse0 =
        reverseAllElements(rewriter, loc, transposedInput, /*dimension*/ 0);
    Value reverse1 =
        reverseAllElements(rewriter, loc, reverse0, /*dimension*/ 1);
    // Move two reversed dimensions to the last for next reverse.
    SmallVector<int64_t, 4> permsval0;
    for (int j = 0; j < inputType.getRank() - 2; ++j)
      permsval0.emplace_back(j + 2);
    for (int j = 0; j < 2; ++j)
      permsval0.emplace_back(j);
    ArrayRef<int64_t> perms(permsval0);
    transposedInput = emitONNXTranspose(loc, rewriter, reverse1, permsval0);
  }
  // 3. Reverse the rest of dimension if spatial rank is odd
  if (spatialRank % 2 != 0) {
    ShapedType tInType = transposedInput.getType().cast<ShapedType>();
    ArrayRef<int64_t> tInShape = tInType.getShape();
    Value reverse0;
    if (tInShape[1] == ShapedType::kDynamic) {
      // When N is unknown dim,
      // reshape "Dn x N x C x D0 xD1 x D2 x ... x Dn-1"
      // to "Dn x 1 x N x C x D0 xD1 x D2 x ... x Dn-1",
      // then, reshape back to original shape after reversed.
      int64_t tInRank = tInShape.size();
      Type tInShapeType =
          RankedTensorType::get({tInRank}, rewriter.getI64Type());
      Value tInShapeVals = create.onnx.shape(tInShapeType, transposedInput);
      SmallVector<int64_t, 6> reshapedShapeVec;
      reshapedShapeVec.emplace_back(tInShape[0]);
      reshapedShapeVec.emplace_back(1);
      for (int i = 1; i < tInType.getRank(); ++i)
        reshapedShapeVec.emplace_back(tInShape[i]);
      Type reshapedType = RankedTensorType::get(reshapedShapeVec, elementType);
      Type firstShapeType = RankedTensorType::get({1}, rewriter.getI64Type());
      Type otherShapeType =
          RankedTensorType::get({tInRank - 1}, rewriter.getI64Type());
      Value oneVal = create.onnx.constantInt64(ArrayRef<int64_t>({1}));
      Value firstShapeVal = create.onnx.slice(
          firstShapeType, tInShapeVals, /* starts */ 0, /* ends */ 1);
      Value otherShapeVals = create.onnx.slice(
          otherShapeType, tInShapeVals, /* starts */ 1, /* ends */ tInRank);
      Type reshapeShapeType =
          RankedTensorType::get({tInRank + 1}, rewriter.getI64Type());
      Value shape = create.onnx.concat(reshapeShapeType,
          ValueRange{firstShapeVal, oneVal, otherShapeVals}, 0);
      transposedInput =
          create.onnx.reshape(reshapedType, transposedInput, shape);
      reverse0 =
          reverseAllElements(rewriter, loc, transposedInput, /*dimension*/ 0);
      reverse0 = create.onnx.reshape(tInType, reverse0, tInShapeVals);
    } else {
      reverse0 =
          reverseAllElements(rewriter, loc, transposedInput, /*dimension*/ 0);
    }

    // Move reversed one dimension to the last.
    SmallVector<int64_t, 4> permsval1;
    for (int j = 0; j < inputType.getRank() - 1; ++j)
      permsval1.emplace_back(j + 1);
    permsval1.emplace_back(0);
    ArrayRef<int64_t> perms(permsval1);
    transposedInput = emitONNXTranspose(loc, rewriter, reverse0, permsval1);
  }
  // 4. Reverse non-spatial dimensions
  SmallVector<int64_t, 4> permsval2;
  for (int i = 0; i < spatialOffset; ++i)
    permsval2.emplace_back(spatialOffset - 1 - i);
  for (int i = 0; i < spatialRank; ++i)
    permsval2.emplace_back(spatialOffset + i);
  ArrayRef<int64_t> perms2(permsval2);
  Value result = emitONNXTranspose(loc, rewriter, transposedInput, perms2);
  return result;
}

// Calculate padding size used in Conv op from pads for ConvTranspose op
ArrayAttr getPadsConvTranspose(PatternRewriter &rewriter, Location loc,
    ArrayAttr kernel, ArrayAttr pads, ArrayAttr dilation) {
  // Calculate pads in generated Conv op by rewriting ConvTranspose op
  // new_pads = kernel -  pads - 1
  // Reference: Dumoulin, Vincent, and Francesco Visin. "A guide to convolution
  // arithmetic for deep learning." arXiv preprint arXiv:1603.07285 (2016).
  SmallVector<int64_t, 4> newPads;
  SmallVector<int64_t, 2> newKernel;
  // If `dilations` is not default [1, 1], `kernel` is updated by inserting
  // spaces in kernel elements
  //   ex. kernel [2, 3] and dilation [2, 2], then new `kernel` is [3, 4]
  for (unsigned int i = 0; i < kernel.size(); ++i)
    newKernel.emplace_back(
        ArrayAttrIntVal(kernel, i) +
        (ArrayAttrIntVal(kernel, i) - 1) * (ArrayAttrIntVal(dilation, i) - 1));

  // Calculate new pads. `kernel` size is doubled for the calculation.
  for (unsigned int i = 0; i < kernel.size() * 2; ++i)
    newPads.emplace_back(
        newKernel[i % kernel.size()] - ArrayAttrIntVal(pads, i) - 1);
  return rewriter.getI64ArrayAttr(newPads);
}

// Check if strides is unit strides
bool hasUnitStrides(ArrayAttr strides) {
  SmallVector<int64_t, 3> vstrides;
  for (unsigned int i = 0; i < strides.size(); ++i)
    vstrides.emplace_back(ArrayAttrIntVal(strides, i));
  return llvm::all_of(vstrides, [](int64_t s) { return s == 1; });
}

ArrayAttr createUnitStrides(PatternRewriter &rewriter, ArrayAttr strides) {
  SmallVector<int64_t, 2> unitStrides;
  for (unsigned int i = 0; i < strides.size(); ++i)
    unitStrides.emplace_back(1);
  return rewriter.getI64ArrayAttr(unitStrides);
}

// Split on the specified axis. The length of each output is one.
ValueRange emitSplitAxisOutputLength1(
    PatternRewriter &rewriter, Location loc, Value input, int64_t axis) {
  onnx_mlir::MultiDialectBuilder<onnx_mlir::OnnxBuilder> create(rewriter, loc);
  ShapedType inputType = input.getType().cast<ShapedType>();
  Type elementType = inputType.getElementType();
  ArrayRef<int64_t> inputShape = inputType.getShape();
  // Create `split` to split each output in `axis` into length 1.
  // Ex. inputShape[axis] = 3, then  onnx.Constant dense<1> : tensor<3xi64>
  SmallVector<int64_t, 1> values(inputShape[axis], 1);
  Value split = create.onnx.constantInt64(ArrayRef(values));
  SmallVector<int64_t> splitShape;
  for (int i = 0; i < inputType.getRank(); ++i) {
    if (i == axis)
      splitShape.emplace_back(1);
    else
      splitShape.emplace_back(inputShape[i]);
  }
  Type splitType = RankedTensorType::get(splitShape, elementType);
  SmallVector<Type, 4> splitTypes(inputShape[axis], splitType);
  ValueRange results =
      create.onnx.split(ArrayRef(splitTypes), input, split, axis);
  return results;
}

// Emit ONNXPadOp to add pads of `size` at end of the `axis`.
Value emitPadsAxisEnd(PatternRewriter &rewriter, Location loc, Value input,
    ArrayRef<int64_t> inputShape, int64_t axis, int64_t size) {
  onnx_mlir::MultiDialectBuilder<onnx_mlir::OnnxBuilder> create(rewriter, loc);
  ShapedType inputType = input.getType().cast<ShapedType>();
  Type elementType = inputType.getElementType();
  SmallVector<int64_t> resultShape;
  for (unsigned int i = 0; i < inputShape.size(); ++i) {
    if (i == axis)
      resultShape.emplace_back(inputShape[i] + size);
    else
      resultShape.emplace_back(inputShape[i]);
  }
  Type resultType = RankedTensorType::get(resultShape, elementType);
  // Specify padding at the end of each axis.
  SmallVector<int64_t, 1> values((int64_t)inputShape.size() * 2, 0);
  values[inputShape.size() + axis] = size;
  Value pads = create.onnx.constantInt64(ArrayRef(values));
  Value result = create.onnx.padZero(resultType, input, pads);
  return result;
}

Value emitConcat(
    PatternRewriter &rewriter, Location loc, ValueRange inputs, int64_t axis) {
  onnx_mlir::MultiDialectBuilder<onnx_mlir::OnnxBuilder> create(rewriter, loc);
  ShapedType inputType = inputs[0].getType().cast<ShapedType>();
  Type elementType = inputType.getElementType();
  ArrayRef<int64_t> inputShape = inputType.getShape();
  int64_t concatAxisSize = 0;
  for (Value v : inputs) {
    ShapedType vType = v.getType().cast<ShapedType>();
    ArrayRef<int64_t> vShape = vType.getShape();
    concatAxisSize += vShape[axis];
  }
  SmallVector<int64_t> concatShape;
  for (unsigned int i = 0; i < inputShape.size(); ++i) {
    if (i == axis)
      concatShape.emplace_back(concatAxisSize);
    else
      concatShape.emplace_back(inputShape[i]);
  }
  Type concatType = RankedTensorType::get(concatShape, elementType);
  Value result = create.onnx.concat(concatType, inputs, axis);
  return result;
}

// Insert pads in specified axis.
Value insertPadAxis(PatternRewriter &rewriter, Location loc, Value input,
    int64_t axis, int64_t padSize) {
  // Split on the specified axis. The length of each output is one.
  ValueRange splitResults =
      emitSplitAxisOutputLength1(rewriter, loc, input, axis);
  // Add pad in split results except last one.
  Value splitLastResults = splitResults.back();
  ValueRange padInputs = splitResults.drop_back();
  SmallVector<Value, 4> padResults;
  for (Value v : padInputs) {
    ArrayRef<int64_t> vShape = v.getType().cast<ShapedType>().getShape();
    padResults.emplace_back(
        emitPadsAxisEnd(rewriter, loc, v, vShape, axis, padSize));
  }
  padResults.emplace_back(splitLastResults);
  // Concat padded results
  Value concatResult = emitConcat(rewriter, loc, ValueRange(padResults), axis);
  return concatResult;
}

// Insert pads between elements in input tensor in spatial dimensions.
// The padding size is strides - 1
Value insertPadsConvTransposeInput(
    PatternRewriter &rewriter, Location loc, Value input, ArrayAttr strides) {
  int64_t spatialOffset = 2;
  for (unsigned int i = 0; i < strides.size(); ++i) {
    input = insertPadAxis(rewriter, loc, input, /*axis*/ spatialOffset + i,
        /*padSize*/ ArrayAttrIntVal(strides, i) - 1);
  }
  return input;
}

// Insert additional padding to output of ConvOp in ConvTransposeOp
Value insertAdditionalPadsConvTranspose(PatternRewriter &rewriter, Location loc,
    ONNXConvOp op, Value input, ArrayAttr outputPaddingAttr,
    ArrayAttr outputShapeAttr) {
  ONNXConvOpShapeHelper shapeHelper(op.getOperation(), {});
  shapeHelper.computeShapeAndAssertOnFailure();
  int inputRank = shapeHelper.getOutputDims().size();
  SmallVector<int64_t, 4> inputShape;
  for (int i = 0; i < inputRank; ++i) {
    int64_t d = shapeHelper.getOutputDims()[i].isLiteral()
                    ? shapeHelper.getOutputDims()[i].getLiteral()
                    : ShapedType::kDynamic;
    inputShape.emplace_back(d);
  }
  SmallVector<int64_t, 2> padSize;
  int64_t attrSize = ArrayAttrSize(outputShapeAttr);
  int64_t offset = inputRank - attrSize;
  for (int i = 0; i < attrSize; ++i) {
    IntegerAttr attr = outputShapeAttr.getValue()[i].cast<IntegerAttr>();
    int64_t size = attr.getValue().getSExtValue() - inputShape[offset + i];
    assert(size >= 0 && "Invalid output_shape attribute");
    padSize.emplace_back(size);
  }
  Value paddedInput = emitPadsAxisEnd(
      rewriter, loc, input, ArrayRef(inputShape), /*axis*/ 2, padSize[0]);
  for (int i = 1; i < attrSize; ++i) {
    ArrayRef<int64_t> paddedInputShape =
        paddedInput.getType().cast<ShapedType>().getShape();
    paddedInput = emitPadsAxisEnd(rewriter, loc, paddedInput, paddedInputShape,
        /*axis*/ 2 + i, padSize[i]);
  }
  return paddedInput;
}
// ConvTransposeOp END

} // namespace onnx_mlir

namespace {
/// Include the patterns defined in the Declarative Rewrite framework.
#include "src/Transform/ONNX/ONNXDecomposeAfter.inc"

struct DecomposeONNXToONNXAfterPass
    : public PassWrapper<DecomposeONNXToONNXAfterPass,
          OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(DecomposeONNXToONNXAfterPass)

  DecomposeONNXToONNXAfterPass(const std::string &target) {
    this->target = target;
  }
  DecomposeONNXToONNXAfterPass(const DecomposeONNXToONNXAfterPass &pass)
      : mlir::PassWrapper<DecomposeONNXToONNXAfterPass,
            OperationPass<func::FuncOp>>() {
    this->target = pass.target;
  }

  StringRef getArgument() const override { return "decompose-onnx-after"; }

  StringRef getDescription() const override {
    return "Decompose ONNX operations into composition of other ONNX "
           "operations after shape inference.";
  }

  Option<std::string> target{*this, "target",
      llvm::cl::desc("Target Dialect to decompose into"), ::llvm::cl::init("")};

  void runOnOperation() final;

  typedef PassWrapper<DecomposeONNXToONNXAfterPass, OperationPass<func::FuncOp>>
      BaseType;
};

void DecomposeONNXToONNXAfterPass::runOnOperation() {
  func::FuncOp function = getOperation();
  MLIRContext *context = &getContext();

  ConversionTarget target(getContext());
  target.addLegalDialect<ONNXDialect, arith::ArithDialect, func::FuncDialect>();

  // These ops will be decomposed into other ONNX ops. Hence, they will not be
  // available after this pass.
  target.addIllegalOp<ONNXConvTransposeOp>();

  RewritePatternSet patterns(context);
  populateWithGenerated(patterns);

  if (failed(applyPartialConversion(function, target, std::move(patterns))))
    signalPassFailure();
}

} // namespace

namespace onnx_mlir {

/*!
 * Create a DecomposeONNXAfter pass.
 */
std::unique_ptr<mlir::Pass> createDecomposeONNXToONNXAfterPass(
    const std::string &target) {
  return std::make_unique<DecomposeONNXToONNXAfterPass>(target);
}

} // namespace onnx_mlir
