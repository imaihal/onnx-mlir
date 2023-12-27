/*
 * SPDX-License-Identifier: Apache-2.0
 */

//===-------- ZLowRewrite.cpp - ZLow Rewrite Patterns ---------------------===//
//
// Copyright 2022 The IBM Research Authors.
//
// =============================================================================
//
// This pass implements optimizations for ZLow operations.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Async/IR/Async.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Interfaces/ViewLikeInterface.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "src/Accelerators/NNPA/Dialect/ZLow/ZLowOps.hpp"
#include "src/Accelerators/NNPA/Pass/NNPAPasses.hpp"
#include "src/Accelerators/NNPA/Support/LayoutHelper.hpp"
#include "src/Dialect/Krnl/KrnlOps.hpp"
#include "src/Dialect/Mlir/DialectBuilder.hpp"

#include <llvm/Support/Debug.h>
#define DEBUG_TYPE "zlow-rewrite"

#include <map>

using namespace mlir;

namespace onnx_mlir {
namespace zlow {

/// Remove unstick if there is no use of its second operand except itself.
class UnstickRemovalPattern : public OpRewritePattern<ZLowUnstickOp> {
public:
  using OpRewritePattern<ZLowUnstickOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(
      ZLowUnstickOp unstickOp, PatternRewriter &rewriter) const override {
    if (!unstickOp.getOut().hasOneUse())
      return failure();
    rewriter.eraseOp(unstickOp);
    return success();
  }
};

/// Remove stick if there is no use of its second operand except itself.
class StickRemovalPattern : public OpRewritePattern<ZLowStickOp> {
public:
  using OpRewritePattern<ZLowStickOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(
      ZLowStickOp stickOp, PatternRewriter &rewriter) const override {
    if (!stickOp.getOut().hasOneUse())
      return failure();
    rewriter.eraseOp(stickOp);
    return success();
  }
};

/// This pattern removes the following (unstick, stick) pair if they use the
/// same layout.
/// ```mlir
///   zlow.unstick(%input, %output) {layout = 3DS}
///   zlow.stick(%output, %res) {layout = 3DS}
/// ```
class UnstickStickRemovalPattern : public OpRewritePattern<ZLowStickOp> {
public:
  using OpRewritePattern<ZLowStickOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      ZLowStickOp stickOp, PatternRewriter &rewriter) const override {
    Value stickInput = stickOp.getX();
    std::optional<StringRef> stickLayout = stickOp.getLayout();

    // Input is a block argument, ignore it.
    if (stickInput.dyn_cast<BlockArgument>())
      return failure();

    // Get UnstickOp that produced the stick input.
    // There is only one UnstickOp per buffer, so stop searching when we get
    // one.
    ZLowUnstickOp unstickOp;
    for (Operation *user : stickInput.getUsers()) {
      ZLowUnstickOp userOp = llvm::dyn_cast<ZLowUnstickOp>(user);
      if (!userOp)
        continue;
      // UnstickOp must be before the stick operation.
      if (userOp.getOut() == stickInput &&
          user->isBeforeInBlock(stickOp.getOperation())) {
        unstickOp = userOp;
        break;
      }
    }
    if (!unstickOp)
      return failure();

    // Stick and Unstick use the same layout.
    std::optional<StringRef> unstickLayout = unstickOp.getLayout();
    if (!stickLayout.has_value() || !unstickLayout.has_value())
      return failure();
    if (stickLayout.value() != unstickLayout.value())
      return failure();

    // Rewrite
    stickOp.getOut().replaceAllUsesWith(unstickOp.getX());
    rewriter.eraseOp(stickOp);

    return success();
  }
};

/// This pattern rewrites
/// ```mlir
///   zlow.unstick(%input, %output)
///   %view = viewOp(%output)
///   zlow.stick(%view, %res)
/// ```
/// by removing `zlow.stick` and replacing `%res` by `%input`, which is
/// constrained by that `%input` and `%res` have the same static shape.
/// This pattern potentially removes `zlow.unstick` and `viewOp` if they are
/// dangling.
///
/// `viewOp` can be any op that inherits ViewLikeOpInterface, e.g.
/// memref.reinterpret_cast, memref.collapse_shape, memref.expand_shape.
//
class StickViewUnstickRemovalPattern : public OpRewritePattern<ZLowStickOp> {
public:
  using OpRewritePattern<ZLowStickOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      ZLowStickOp stickOp, PatternRewriter &rewriter) const override {
    Value stickInput = stickOp.getX();

    // Do not handle NCHW layout stickification that transposes data
    // internally.
    std::string stickLayout = stickOp.getLayout().value().str();
    if (stickLayout == LAYOUT_NCHW)
      return failure();

    // Input is a block argument, ignore it.
    if (stickInput.dyn_cast<BlockArgument>())
      return failure();

    // Input must have no affine layout. In other words, it has been normalized.
    if (hasNonIdentityLayout(stickInput.getType()))
      return failure();

    // Input is a view.
    ViewLikeOpInterface viewOp =
        llvm::dyn_cast<ViewLikeOpInterface>(stickInput.getDefiningOp());
    if (!viewOp)
      return failure();
    // Get the source of the view.
    Value viewSource = viewOp.getViewSource();

    // Get UnstickOp that unstickifies the view source.
    // There is only one UnstickOp per buffer, so stop searching when we get
    // one.
    ZLowUnstickOp unstickOp;
    for (Operation *user : viewSource.getUsers()) {
      ZLowUnstickOp userOp = llvm::dyn_cast<ZLowUnstickOp>(user);
      if (!userOp)
        continue;
      // Do not handle NCHW layout stickification that transposes data
      // internally.
      std::string unstickLayout = userOp.getLayout().value().str();
      if (unstickLayout == LAYOUT_NCHW)
        continue;
      // UnstickOp must be before the view operation.
      if (userOp.getOut() == viewSource &&
          user->isBeforeInBlock(viewOp.getOperation())) {
        unstickOp = userOp;
        break;
      }
    }
    if (!unstickOp)
      return failure();

    // Match shapes.
    Value stickRes = stickOp.getOut();
    Value unstickInput = unstickOp.getX();
    MemRefType stickResType = stickRes.getType().dyn_cast<MemRefType>();
    MemRefType unstickInputType = unstickInput.getType().dyn_cast<MemRefType>();
    if (!stickResType.hasStaticShape() ||
        (stickResType.getShape() != unstickInputType.getShape()))
      return failure();

    // Rewrite
    rewriter.eraseOp(stickOp);
    stickRes.replaceAllUsesWith(unstickInput);
    // Remove the view op if there is no use.
    if (viewOp.getOperation()->getResults()[0].use_empty())
      rewriter.eraseOp(viewOp);
    return success();
  }
};

// clang-format off
///
/// * Pattern to rewrite
/// ```
/// zlow.unstick -> affine.for (affine.load -> affine.store) -> zlow.stick
///    |                            |
///    |                            '--------> affine.store) -> zlow.stick
///    |
///    '----------> affine.for (affine.load -> affine.store) -> zlow.stick
///                                                              ^
/// zlow.unstick -> affine.for (affine.load -> affine.store) ----'
/// ```
///
/// * Example:
///
/// Consider the following code: 
/// ```mlir
/// zlow.unstick(%stick, %A) {layout = "2D"}: memref<2x3xf16, #map2D>, memref<2x3xf32>
/// affine.for
///   %a = affine.load(%A, %load_indices) : memref<2x3xf32>
///   affine.store(%a, %B, %store_indices) : memref<4x5x6xf32>
/// %res = memref.alloc() : memref<4x5x6xf16, #map3D>
/// zlow.stick(%B, %res) {layout = "3D"}: memref<4x5x6xf32>, memref<4x5x6xf16,
/// #map3D>
/// ```
/// `%stick` memref is unstickified and shuffled by the pair of (affine.load,affine.store),
/// then stickified again. It said data are transfered from a stickified memref
/// into another stickified memref via a chain of affine transformation.
///
/// The above code can be rewritten into the following code:
/// ```mlir
/// %res = memref.alloc() : memref<4x5x6xf16, #map3D>
/// affine.for
///   %a = affine.load(%stick, %load_indices) : memref<2x3xf16, #map2D>
///   affine.store(%a, %res, %store_indices) : memref<4x5x6xf16, #map3D>
/// ```
/// where data will be directly loaded from / stored to stickified memref.
///
/// This pattern is often found in code generated for data transformation such
/// as Transpose, Concat, and Split.
///
/// * Why does this rewriting work?
/// 
/// - This rewriting depends on the fact that `zlow.stick` and `zlow.unstick`
/// maintain an affine map that maps one element in a memref to an element in
/// another memref. Those maps are `#map2D` and `#map3D` in the above example.
/// Combined with affine.load and affine.store, one element in a stickified
/// memref can be forwarded directly into an element in another stickifired
/// memref without `zlow.stick` and `zlow.unstick`.
///
/// - The shape of the input and output memrefs of `zlow.stick`/`zlow.unstick`
/// are the same except the case of layout NCHW. In case of NCHW, dimensions are
/// permuted, so we handle NCHW as a special case in this rewriting.
/// ```mlir
///  zlow.stick(%X, %res) {layout = "NCHW"}: memref<1x3x5x7xf32>, memref<1x5x7x3xf16, #mapNHWC>
///  ```
///  Shape of `%X` is in NCHW while shape of `%res` is in NHWC.
//
/// ```mlir
/// zlow.unstick(%X, %res) {layout = "NCHW"}: memref<1x5x7x3xf16, #mapNHWC>, memref<1x3x5x7xf32>
/// ```
///  Shape of `%X` is in NHWC while shape of `%res` is in NCHW.
///
/// * Limitations
///
/// - Unstickified memrefs (`%A` and `%B`) must have no affine map.
/// Theoretically, we could support affine map on unstickified memrefs by
/// composing affine-map.

// clang-format on

class UnstickLoadStoreStickRemovalPattern
    : public OpRewritePattern<ZLowUnstickOp> {
public:
  using OpRewritePattern<ZLowUnstickOp>::OpRewritePattern;

  UnstickLoadStoreStickRemovalPattern(MLIRContext *context,
      llvm::SmallDenseSet<ZLowStickOp, 4> &removableStickOps_)
      : OpRewritePattern(context, /*benefit=*/1),
        removableStickOps(removableStickOps_) {}

  LogicalResult matchAndRewrite(
      ZLowUnstickOp unstickOp, PatternRewriter &rewriter) const override {
    Location loc = unstickOp.getLoc();
    Operation *op = unstickOp.getOperation();
    MLIRContext *ctx = unstickOp.getContext();

    // stickifiedMemRef has affine layout, e.g. MemRef<1x3x5xf32, #map>
    Value stickifiedMemRef = unstickOp.getX();
    // cpuMemRef has no affine layout, e.g. MemRef<1x3x5xf32>
    Value cpuMemRef = unstickOp.getOut();
    std::string unstickLayout = unstickOp.getLayout().value().str();
    bool unstickNCHWLayout = (unstickLayout == LAYOUT_NCHW);

    // Common types.
    Type stickifiedElementType =
        stickifiedMemRef.getType().cast<MemRefType>().getElementType();
    Type cpuElementType =
        cpuMemRef.getType().cast<MemRefType>().getElementType();

    // Stickified Memref must have affine layout to access elements.
    if (!hasNonIdentityLayout(stickifiedMemRef.getType()))
      return rewriter.notifyMatchFailure(op, [&](::mlir::Diagnostic &diag) {
        diag << "Stickified Memref has no affine layout";
      });

    // Do not support affine layout in the CPU Memref at this moment.
    if (hasNonIdentityLayout(cpuMemRef.getType()))
      return rewriter.notifyMatchFailure(op, [&](::mlir::Diagnostic &diag) {
        diag << "Unstickified Memref has affine layout";
      });

    // Do not support layout 1D and 2DS since their access index functions are
    // incorrect: https://github.com/onnx/onnx-mlir/issues/1940
    if ((unstickLayout == LAYOUT_1D) || (unstickLayout == LAYOUT_2DS))
      return rewriter.notifyMatchFailure(op, [&](::mlir::Diagnostic &diag) {
        diag << "Unsupport layout 1D and 2DS";
      });

    // 1. Match pattern: data flows from zlow.unstick to zlow.stick via
    // affine.load and affine.store.
    // - Support sharing load-from/store-to zlow.unstick/zlow.stick.
    //
    //  zlow.unstick -> affine.for (affine.load -> affine.store) -> zlow.stick
    //     |                            |
    //     |                            '--------> affine.store) -> zlow.stick
    //     |
    //     '----------> affine.for (affine.load -> affine.store) -> zlow.stick
    //                                                               ^
    //  zlow.unstick -> affine.for (affine.load -> affine.store) ----'
    //

    // All consumers of zlow.unstick must be affine.load.
    SmallVector<affine::AffineLoadOp, 4> loadOps;
    if (!matchAndCollectAffineLoad(unstickOp, cpuMemRef, loadOps))
      return rewriter.notifyMatchFailure(op, [&](::mlir::Diagnostic &diag) {
        diag << "Failed to match AffineLoadOp";
      });

    // All consumers of affine.load must be affine.store.
    // affine.store must store to a Memref allocated by memref.alloc.
    SmallVector<affine::AffineStoreOp, 4> storeOps;
    if (!matchAndCollectAffineStore(loadOps, storeOps))
      return rewriter.notifyMatchFailure(op, [&](::mlir::Diagnostic &diag) {
        diag << "Failed to match AffineStoreOp";
      });

    // Each affine.store is connected to one zlow.stick.
    std::map<affine::AffineStoreOp, ZLowStickOp> StoreOpStickOpMap;
    SmallVector<ZLowStickOp, 4> stickOps;
    if (!matchAndCollectStickOp(storeOps, stickOps, StoreOpStickOpMap))
      return rewriter.notifyMatchFailure(op, [&](::mlir::Diagnostic &diag) {
        diag << "Two ZLowStickOp linked to an AffineStoreOp";
      });

    // 2. Rewrite
    // - Rewrite AffineLoadOp to use stickified Memref directly.
    MultiDialectBuilder<AffineBuilder> create(rewriter, loc);
    for (affine::AffineLoadOp loadOp : loadOps) {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointAfter(loadOp);
      // Clone loadOp with new Memref, indices and return type.
      IRMapping operandMap;
      operandMap.map(loadOp.getMemref(), stickifiedMemRef);
      Operation *clonedOp = rewriter.clone(*loadOp.getOperation(), operandMap);
      clonedOp->getResult(0).setType(stickifiedElementType);
      // Permute affine_map in case of NCHW layout.
      if (unstickNCHWLayout) {
        AffineMapAttr oldMap = loadOp.getAffineMapAttr();
        // NCHW -> NHWC
        SmallVector<unsigned, 4> permutation = {0, 2, 3, 1};
        AffineMap permuteMap = AffineMap::getPermutationMap(permutation, ctx);
        AffineMapAttr newMap =
            AffineMapAttr::get(permuteMap.compose(oldMap.getValue()));
        clonedOp->setAttr(affine::AffineLoadOp::getMapAttrStrName(), newMap);
      }
      // This DummyOp is used to make the intermediate generated code valid. It
      // wil be removed automatically via canonicalization.
      Value dummyConverter = rewriter.create<ZLowDummyOp>(
          loc, cpuElementType, clonedOp->getResult(0));
      rewriter.replaceOp(loadOp, {dummyConverter});
    }

    // - Rewrite AffineStoreOp to use stickified Memref directly.
    for (affine::AffineStoreOp storeOp : storeOps) {
      Value storeMemref = storeOp.getMemref();
      Value storeValue = storeOp.getValue();
      ZLowStickOp myStickOp = StoreOpStickOpMap[storeOp];
      Value stickMemref = myStickOp.getOut();
      std::string stickLayout = myStickOp.getLayout().value().str();
      bool stickNCHWLayout = (stickLayout == LAYOUT_NCHW);

      // Move stickMemref's AllocOp up before affine.for so that it
      // dominates its uses. A good place is just after storeMemref's AllocOp.
      //
      // Get AllocOps that allocated storeMemref and stickMemref.
      Operation *storeAllocOp = storeMemref.getDefiningOp();
      Operation *stickAllocOp = stickMemref.getDefiningOp();
      // stickAllocOp should be after storeAllocOp, since dimensions come from
      // storeAllocOp according to the definition of zlow.stick.
      Operation *justMovedOp = nullptr;
      // Move AllocOp's operands first.
      for (unsigned i = 0; i < stickAllocOp->getNumOperands(); ++i) {
        Value oprd = stickAllocOp->getOperand(i);
        if (isa<BlockArgument>(oprd))
          continue;
        Operation *opToMove = oprd.getDefiningOp();
        // Do not move, it is potentially used by storeAllocOp and it is a good
        // place already.
        if (opToMove->isBeforeInBlock(storeAllocOp))
          continue;
        if (justMovedOp)
          opToMove->moveAfter(justMovedOp);
        else
          opToMove->moveAfter(storeAllocOp);
        justMovedOp = opToMove;
      }
      // Move AllocOp.
      if (justMovedOp)
        stickAllocOp->moveAfter(justMovedOp);
      else
        stickAllocOp->moveAfter(storeAllocOp);

      // Replace storeOp.
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointAfter(storeOp);
      // This DummyOp is used to make the intermediate generated code valid. It
      // will be removed automatically via canonicalization.
      Value dummyConverter =
          rewriter.create<ZLowDummyOp>(loc, stickifiedElementType, storeValue);
      // Clone storeOp with new Memref, Value, and Indices.
      IRMapping operandMap;
      operandMap.map(storeOp.getMemref(), stickMemref);
      operandMap.map(storeOp.getValue(), dummyConverter);
      Operation *clonedOp = rewriter.clone(*storeOp.getOperation(), operandMap);
      // Permute affine_map in case of NCHW layout.
      if (stickNCHWLayout) {
        AffineMapAttr oldMap = storeOp.getAffineMapAttr();
        // NCHW -> NHWC
        SmallVector<unsigned, 4> permutation = {0, 2, 3, 1};
        AffineMap permuteMap = AffineMap::getPermutationMap(permutation, ctx);
        AffineMapAttr newMap =
            AffineMapAttr::get(permuteMap.compose(oldMap.getValue()));
        clonedOp->setAttr(affine::AffineStoreOp::getMapAttrStrName(), newMap);
      }
      rewriter.eraseOp(storeOp);
    }

    // Remove ZLowUnstickOp.
    rewriter.eraseOp(unstickOp);
    // Copy ZLowStickOp to the removableStickOps. We cannot remove it now
    // because there are potentially other AffineStoreOps using to it.
    for (ZLowStickOp stickOp : stickOps)
      removableStickOps.insert(stickOp);
    return success();
  }

private:
  llvm::SmallDenseSet<ZLowStickOp, 4> &removableStickOps;

  // Collect affine.load operations that connect to zlow.unstick.
  bool matchAndCollectAffineLoad(ZLowUnstickOp unstickOp, Value loadMemref,
      SmallVectorImpl<affine::AffineLoadOp> &loadOps) const {
    for (Operation *user : loadMemref.getUsers()) {
      if (user == unstickOp.getOperation())
        continue;
      if (auto loadOp = llvm::dyn_cast<affine::AffineLoadOp>(user))
        loadOps.emplace_back(loadOp);
      else
        return false;
    }

    return (loadOps.size() != 0);
  }

  // Collect affine.store operations that connect to affine.load.
  bool matchAndCollectAffineStore(
      const SmallVectorImpl<affine::AffineLoadOp> &loadOps,
      SmallVectorImpl<affine::AffineStoreOp> &storeOps) const {
    for (affine::AffineLoadOp loadOp : loadOps) {
      Value loadValue = loadOp.getValue();
      for (Operation *user : loadValue.getUsers()) {
        if (user == loadOp.getOperation())
          continue;
        if (auto storeOp = llvm::dyn_cast<affine::AffineStoreOp>(user)) {
          // Check unstick -> load -> store -> stick.
          if (!matchUnstickLoadStoreStick(storeOp))
            return false;
          storeOps.emplace_back(storeOp);
        } else
          return false;
      }
    }

    // Not match if there is a "strange" AffineStoreOp that stores to MemRef.
    // The AffineStoreOp is strange in the sense that it does not store a value
    // that comes from AffineLoadOp. For example, in PadOp, there is a loop that
    // directly stores a zero constant to a MemRef. In that case there is no way
    // to create a F16 constant in Z.
    // TODO: Support this situation.
    for (affine::AffineStoreOp storeOp : storeOps) {
      Value destMemref = storeOp.getMemref();
      for (Operation *user : destMemref.getUsers()) {
        if (user == storeOp.getOperation())
          continue;
        if (auto otherStoreOp = llvm::dyn_cast<affine::AffineStoreOp>(user)) {
          if (llvm::all_of(storeOps, [&](affine::AffineStoreOp op) {
                return (op != otherStoreOp);
              })) {
            // Check unstick -> load -> store -> stick.
            if (!matchUnstickLoadStoreStick(otherStoreOp))
              return false;
          }
        }
      }
    }

    return (storeOps.size() != 0);
  }

  // Collect zlow.stick operations that connect to affine.store.
  bool matchAndCollectStickOp(
      const SmallVectorImpl<affine::AffineStoreOp> &storeOps,
      SmallVectorImpl<ZLowStickOp> &stickOps,
      std::map<affine::AffineStoreOp, ZLowStickOp> &StoreOpStickOpMap) const {
    for (affine::AffineStoreOp storeOp : storeOps) {
      ZLowStickOp myStickOp;
      Value destMemref = storeOp.getMemref();
      for (Operation *user : destMemref.getUsers()) {
        if (user == storeOp.getOperation())
          continue;
        if (llvm::dyn_cast<affine::AffineStoreOp>(user))
          continue;
        if (auto stick = llvm::dyn_cast<ZLowStickOp>(user)) {
          // Do not support layout 1D and 2DS since their access index
          // functions are incorrect:
          // https://github.com/onnx/onnx-mlir/issues/1940
          std::string stickLayout = stick.getLayout().value().str();
          if ((stickLayout == LAYOUT_1D) || (stickLayout == LAYOUT_2DS))
            return false;

          if (myStickOp)
            return false;
          else
            myStickOp = stick;
        } else
          return false;
      }
      stickOps.emplace_back(myStickOp);
      StoreOpStickOpMap[storeOp] = myStickOp;
    }
    return (stickOps.size() != 0);
  }

  // Check this sequence: unstick -> load -> store -> stick.
  bool matchUnstickLoadStoreStick(affine::AffineStoreOp storeOp) const {
    Value destMemref = storeOp.getMemref();
    Value storeValue = storeOp.getValue();

    // Store's input must be defined by a memref.alloc.
    if (destMemref.isa<BlockArgument>())
      return false;
    Operation *allocOp = destMemref.getDefiningOp();
    if (!isa<memref::AllocOp>(allocOp))
      return false;

    // Users of AffineStoreOp's MemRef must be StoreOp and StickOp.
    if (!matchMultipleStoreSingleStick(destMemref))
      return false;

    // Check if the store value is from AffineLoadOp or not.
    if (isa<BlockArgument>(storeValue))
      return false;
    if (auto loadOp =
            dyn_cast<affine::AffineLoadOp>(storeValue.getDefiningOp())) {
      // Check if loading from MemRef that is unstickified.
      Value memRef = loadOp.getMemref();
      if (!matchMultipleLoadSingleUnstick(memRef))
        return false;
    } else
      return false;

    return true;
  }

  // Users of MemRef must be StoreOp and StickOp.
  bool matchMultipleStoreSingleStick(Value memRef) const {
    if (isa<BlockArgument>(memRef))
      return false;
    ZLowStickOp stickOp;
    affine::AffineStoreOp storeOp;
    for (Operation *user : memRef.getUsers()) {
      // At least one StoreOp.
      if (auto store = llvm::dyn_cast<affine::AffineStoreOp>(user)) {
        storeOp = store;
        continue;
      }
      // Only one StickOp.
      if (auto stick = llvm::dyn_cast<ZLowStickOp>(user)) {
        if (stickOp)
          return false;
        stickOp = stick;
        continue;
      }
      return false;
    }
    return (storeOp && stickOp);
  }

  // Users of MemRef must be LoadOp and UnstickOp.
  bool matchMultipleLoadSingleUnstick(Value memRef) const {
    if (isa<BlockArgument>(memRef))
      return false;
    ZLowUnstickOp unstickOp;
    affine::AffineLoadOp loadOp;
    for (Operation *user : memRef.getUsers()) {
      // At least one LoadOp.
      if (auto load = dyn_cast<affine::AffineLoadOp>(user)) {
        loadOp = load;
        continue;
      }
      // Only one UnstickOp.
      if (auto unstick = dyn_cast<ZLowUnstickOp>(user)) {
        if (unstickOp)
          return false;
        else
          unstickOp = unstick;
        continue;
      }
      return false;
    }
    return (loadOp && unstickOp);
  }
};

/// This pattern rewrites alloc and dealloc ops used in the region of async
/// execute op. The input values for the async execute op need to be deallocated
/// after completing the threads. The result value need to be allocated outside
/// of the region (in main thread) and deallocated after used.
///
/// Example (ZLowIR):
/// - Input values (%arg0, %alloc) are allocated before async.execute. They need
/// to be deallocated after async.await.
/// - Result value (%alloc_8) is allocated in async.execute. It is used in
/// krnl.iterate for Concat. It needs to be deallocated after the krnl.iterate.
///
/// This pattern inserts dealloc op for %alloc after krnl.iterate. The %alloc
/// can be deallocated by other threads. So the dealloc op is inserted only when
/// it is not deallocated yet. This pattern moves %alloc_8 before async.execute
/// and inserts dealloc op to deallocate it.
// clang-format off
/// ```mlir
///    %alloc = memref.alloc() {alignment = 16 : i64} : memref<512x512xf32>
///        :
///    %token, %bodyResults = async.execute -> !async.value<memref<512x512xf32>> {
///      %alloc_4 = memref.alloc() {...
///      "zlow.stick"(%arg0, %alloc_4) {...
///      %alloc_5 = memref.alloc() {...
///      "zlow.stick"(%alloc, %alloc_5) {...
///      %alloc_6 = memref.alloc() {...
///       :
///      "zlow.matmul"(%alloc_4, %alloc_5, ..., %alloc_6) {...
///      %alloc_8 = memref.alloc() {...
///      "zlow.unstick"(%alloc_6, %alloc_8) {...
///      async.yield %alloc_8 : memref<512x512xf32>
///    }
///      :
///    %2 = async.await %bodyResults : !async.value<memref<512x512xf32>>
///      :
///    %4:2 = krnl.define_loops 2
///    krnl.iterate(%4#0, %4#1) with (...   ){
///      %6:2 = krnl.get_induction_var_value(%4#0, %4#1) : (...
///      %7 = krnl.load %2[%6#0, %6#1] : memref<512x512xf32>
///      krnl.store %7, %alloc_3[%6#0, %6#1] : memref<512x1024xf32>
///    }
/// ```
// clang-format on

class InsertDeallocForAsyncExecRegionPattern
    : public OpRewritePattern<async::ExecuteOp> {
public:
  using OpRewritePattern<async::ExecuteOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      async::ExecuteOp executeOp, PatternRewriter &rewriter) const override {

    SmallVector<memref::AllocOp, 4> regionAllocOps;
    SmallVector<Value, 4> inputValues;
    for (Operation &op : executeOp.getBodyRegion().getOps()) {
      // Get allocOps in body of async.execute
      if (auto allocOp = dyn_cast<memref::AllocOp>(op))
        regionAllocOps.push_back(allocOp);
      // Get input values used in async.execute.
      for (unsigned i = 0; i < op.getNumOperands(); ++i) {
        Operation *defOp = op.getOperand(i).getDefiningOp();
        auto allocOp = dyn_cast_or_null<memref::AllocOp>(defOp);
        if (allocOp) {
          if (allocOp->getBlock() != executeOp.getBody())
            inputValues.push_back(allocOp.getResult());
        }
      }
    }
    // Get the allocOp for the result value of async.execute and move it before
    // async.execute. The allocOp is an operand of async.yeild.
    async::YieldOp yieldOp =
        cast<async::YieldOp>(executeOp.getBody()->getTerminator());
    // Currently support single operand.
    if (yieldOp.getOperands().size() > 1)
      return failure();
    Value yieldOperand = yieldOp.getOperands()[0];
    auto yAllocOp =
        dyn_cast_or_null<memref::AllocOp>(yieldOperand.getDefiningOp());
    if (yAllocOp) {
      if (llvm::none_of(regionAllocOps,
              [&](memref::AllocOp aop) { return aop == yAllocOp; }))
        return failure();
      else
        yAllocOp->moveBefore(executeOp);
    } else {
      return failure();
    }

    // Get users of async.await
    // Currently support single body result.
    if (executeOp.getBodyResults().size() > 1)
      return failure();
    Value executeOpResult = executeOp.getBodyResults()[0];
    SmallVector<Operation *, 4> awaitOutUsers;
    for (Operation *user : executeOpResult.getUsers()) {
      if (auto awaitOp = llvm::dyn_cast<async::AwaitOp>(user)) {
        Value awaitOpOut = awaitOp.getResult();
        for (Operation *awaitUser : awaitOpOut.getUsers())
          awaitOutUsers.push_back(awaitUser);
      }
    }

    // Insert deallocOp for the result value after it is used. When it is used
    // in different block from allocation (This happens when it is used in the
    // loop), deallocOp is inserted in parent block.
    Operation *insertionPointOp;
    if (yAllocOp->getBlock() != awaitOutUsers[0]->getBlock())
      insertionPointOp = awaitOutUsers[0]->getParentOp();
    else
      insertionPointOp = awaitOutUsers[0];
    Location loc = insertionPointOp->getLoc();
    MultiDialectBuilder<MemRefBuilder> create(rewriter, loc);
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointAfter(insertionPointOp);
    create.mem.dealloc(yAllocOp.getResult());

    // Insert deallocOp for the input values if it is not deallocated yet.
    for (Value inVal : inputValues) {
      if (llvm::none_of(inVal.getUsers(),
              [&](Operation *user) { return isa<memref::DeallocOp>(user); }))
        create.mem.dealloc(inVal);
    }

    return success();
  }
};

/*!
 *  Function pass that optimizes ZLowIR.
 */
class ZLowRewritePass
    : public PassWrapper<ZLowRewritePass, OperationPass<func::FuncOp>> {
public:
  StringRef getArgument() const override { return "zlow-rewrite"; }

  StringRef getDescription() const override { return "Rewrite ZLow Ops."; }

  void runOnOperation() override {
    Operation *function = getOperation();

    llvm::SmallDenseSet<ZLowStickOp, 4> removableStickOps;
    ConversionTarget target(getContext());
    RewritePatternSet patterns(&getContext());
    patterns.insert<StickRemovalPattern>(&getContext());
    patterns.insert<UnstickRemovalPattern>(&getContext());
    patterns.insert<UnstickStickRemovalPattern>(&getContext());
    patterns.insert<StickViewUnstickRemovalPattern>(&getContext());
    patterns.insert<UnstickLoadStoreStickRemovalPattern>(
        &getContext(), removableStickOps);
    // patterns.insert<InsertDeallocForAsyncExecRegionPattern>(&getContext());

    if (failed(applyPatternsAndFoldGreedily(function, std::move(patterns))))
      return signalPassFailure();

    // Remove ZLowStickOp that were marked "removable".
    for (ZLowStickOp stickOp : removableStickOps) {
      if (!stickOp) // removed, continue.
        continue;
      stickOp.getOperation()->erase();
    }
  }
};

std::unique_ptr<Pass> createZLowRewritePass() {
  return std::make_unique<ZLowRewritePass>();
}

} // namespace zlow
} // namespace onnx_mlir
