add_executable(onnx-mlir-opt ONNXMLIROpt.cpp)
add_dependencies(onnx-mlir-opt OMKrnlOpsInc OMONNXOpsInc)

target_include_directories(onnx-mlir-opt PRIVATE ${ONNX_MLIR_SRC_ROOT})
target_include_directories(onnx-mlir-opt PRIVATE ${ONNX_MLIR_BIN_ROOT})

set(ONNX_MLIR_LD_PRELOAD_onnx-mlir-opt "" CACHE STRING "" FORCE)
if(BUILD_SHARED_LIBS)
  message(STATUS "To run dynamically linked onnx-mlir-opt, you must specify:")
  message(STATUS "LD_PRELOAD=${ONNX_MLIR_LD_PRELOAD_onnx-mlir-opt}")
endif()

target_link_libraries(onnx-mlir-opt
        ${OMLibs}
        ${MLIRLibs}
        onnx)
