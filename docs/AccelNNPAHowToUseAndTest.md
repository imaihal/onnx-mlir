<!--- SPDX-License-Identifier: Apache-2.0 -->

# Build and test for Accelerator NNPA

## Build

The following CMake variable is required to build onnx-mlir for NNPA.

- `-DONNX_MLIR_ACCELERATORS=NNPA`

## Test

Currently numerical tests are provided for NNPA. To run the same tests with CPU, appropriate ATOL and RTOL are required to pass the tests. They can be set as environment variable as shown below. Some tests with CPU are not supported in zDNN library used by NNPA. So, NNPA specific tests are provided in `test/accelerator/NNPA/numrical`. 

- The same numerical tests with CPU
  TEST_ATOL=0.01 TEST_RTOL=0.05 cmake --build . --config Release --target check-onnx-numerical
  
- zDNN supported tests for NNPA
  cmake --build . --config Release --target check-onnx-numerical-nnpa

 