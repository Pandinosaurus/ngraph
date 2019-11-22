//*****************************************************************************
// Copyright 2017-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

// NOTE: This file follows nGraph format style.
// Follows nGraph naming convention for public APIs only, else MLIR naming convention.

#include "cpu_runtime.hpp"
#include "contrib/mlir/backend/cpu/cpu_backend.hpp"
#include "ngraph/check.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <mlir/ExecutionEngine/ExecutionEngine.h>
#include <mlir/ExecutionEngine/MemRefUtils.h>
#include <mlir/ExecutionEngine/OptUtils.h>
#include <mlir/IR/Function.h>

#include <mlir/Dialect/LLVMIR/LLVMDialect.h>

#include "ngraph/runtime/cpu/cpu_kernels.hpp"
#include "ngraph/runtime/cpu/mkldnn_utils.hpp"

using llvm::SmallVector;
using llvm::StringRef;
using llvm::ArrayRef;

using namespace ngraph;
using namespace ngraph::runtime::ngmlir;

/// Callback for Softmax
template <int N>
void __mlir_mkldnn_softmax(void* input, void* output, const size_t softmax_axis)
{
    auto memRefInput = reinterpret_cast<StridedMemRef<N>*>(input);
    auto memRefOutput = reinterpret_cast<StridedMemRef<N>*>(output);
    mkldnn::memory::dims dims(N);
    mkldnn::memory::dims strides(N);
    for (auto i = 0; i < N; i++)
    {
        dims[i] = memRefInput->shape[i];
        strides[i] = memRefInput->strides[i];
    }

    // build mkldnn primitive and execute
    mkldnn::memory::data_type dtype = mkldnn::memory::data_type::f32;
    auto input_desc = mkldnn::memory::desc(dims, dtype, strides);
    auto softmax_desc =
        mkldnn::softmax_forward::desc(mkldnn::prop_kind::forward_scoring, input_desc, softmax_axis);
    mkldnn::primitive_attr attr;
    mkldnn::engine cpu_engine(mkldnn::engine::kind::cpu, 0);
    auto softmax_pd = mkldnn::softmax_forward::primitive_desc(softmax_desc, attr, cpu_engine);
    mkldnn::softmax_forward softmax(softmax_pd);

    mkldnn::memory in{
        softmax_pd.src_desc(), cpu_engine, memRefInput->basePtr + memRefInput->offset};
    mkldnn::memory out{
        softmax_pd.dst_desc(), cpu_engine, memRefOutput->basePtr + memRefOutput->offset};

    std::unordered_map<int, mkldnn::memory> exec_args = {{MKLDNN_ARG_SRC, in},
                                                         {MKLDNN_ARG_DST, out}};

    mkldnn::stream s(cpu_engine);
    try
    {
        softmax.execute(s, exec_args);
        s.wait();
    }
    catch (const mkldnn::error& e)
    {
        throw ngraph_error("Could not run mkdnn primitive " + std::string(e.message));
    }
}

extern "C" NGRAPH_API void __mlir_mkldnn_softmax_2d(StridedMemRef<1>* input,
                                                    StridedMemRef<1>* output,
                                                    const size_t softmax_axis)
{
    __mlir_mkldnn_softmax<2>(input, output, softmax_axis);
}

extern "C" NGRAPH_API void __mlir_mkldnn_softmax_4d(StridedMemRef<1>* input,
                                                    StridedMemRef<1>* output,
                                                    const size_t softmax_axis)
{
    __mlir_mkldnn_softmax<4>(input, output, softmax_axis);
}

/// Callback for MatMul
extern "C" NGRAPH_API void __mlir_cblas_sgemm(
    void* matAPtr, void* matBPtr, void* matCPtr, const bool transposeA, const bool transposeB)
{
    auto memRefmatA = reinterpret_cast<StridedMemRef<2>*>(matAPtr);
    auto memRefmatB = reinterpret_cast<StridedMemRef<2>*>(matBPtr);
    auto memRefmatC = reinterpret_cast<StridedMemRef<2>*>(matCPtr);

    auto m = memRefmatA->shape[0];
    auto k = memRefmatA->shape[1];
    auto n = memRefmatB->shape[1];
    auto lda = memRefmatA->shape[1];
    auto ldb = memRefmatB->shape[1];

    if (transposeA)
    {
        m = memRefmatA->shape[1];
        k = memRefmatA->shape[0];
    }
    if (transposeB)
    {
        n = memRefmatB->shape[0];
    }

    auto ldc = n;

    cblas::cblas_sgemm(cblas::Layout::RowMajor,
                       transposeA ? cblas::Transpose::Transpose : cblas::Transpose::None,
                       transposeB ? cblas::Transpose::Transpose : cblas::Transpose::None,
                       m,
                       n,
                       k,
                       1.0f,
                       memRefmatA->basePtr + memRefmatA->offset,
                       std::max<size_t>(1, lda),
                       memRefmatB->basePtr + memRefmatB->offset,
                       std::max<size_t>(1, ldb),
                       0.0f,
                       memRefmatC->basePtr + memRefmatC->offset,
                       std::max<size_t>(1, ldc));
}

/// Callback for Gemm
extern "C" NGRAPH_API void __mlir_cblas_sgemm_with_bias(void* matAPtr,
                                                        void* matBPtr,
                                                        void* matCPtr,
                                                        void* matOutPtr,
                                                        const bool transposeA,
                                                        const bool transposeB,
                                                        const size_t m,
                                                        const size_t n,
                                                        const size_t k,
                                                        const size_t lda,
                                                        const size_t ldb,
                                                        const size_t ldc,
                                                        const float alpha,
                                                        const float beta,
                                                        const int broadcastHint)
{
    auto* matA = *(reinterpret_cast<float**>(matAPtr));
    auto* matB = *(reinterpret_cast<float**>(matBPtr));
    auto* matC = *(reinterpret_cast<float**>(matCPtr));
    auto* matOut = *(reinterpret_cast<float**>(matOutPtr));

    cblas::cblas_sgemm(cblas::Layout::RowMajor,
                       transposeA ? cblas::Transpose::Transpose : cblas::Transpose::None,
                       transposeB ? cblas::Transpose::Transpose : cblas::Transpose::None,
                       m,
                       n,
                       k,
                       alpha,
                       matA,
                       std::max<size_t>(1, lda),
                       matB,
                       std::max<size_t>(1, ldb),
                       0.0f,
                       matOut,
                       std::max<size_t>(1, ldc));

    if (broadcastHint == 0)
    {
        std::vector<float> ones(m, 1.0f);
        cblas::cblas_sgemm(cblas::Layout::RowMajor,
                           cblas::Transpose::None,
                           cblas::Transpose::None,
                           m,
                           n,
                           1,
                           beta,
                           ones.data(),
                           1,
                           matC,
                           std::max<size_t>(1, n),
                           1.0f,
                           matOut,
                           std::max<size_t>(1, ldc));
    }
    else if (broadcastHint == 1)
    {
        std::vector<float> ones(n, 1.0f);
        cblas::cblas_sgemm(cblas::Layout::RowMajor,
                           cblas::Transpose::None,
                           cblas::Transpose::None,
                           m,
                           n,
                           1,
                           beta,
                           matC,
                           1,
                           ones.data(),
                           std::max<size_t>(1, n),
                           1.0f,
                           matOut,
                           std::max<size_t>(1, ldc));
    }
    else if (broadcastHint == 2)
    {
        std::vector<float> ones(m, 1.0f);
        std::vector<float> bias(n, *matC);
        cblas::cblas_sgemm(cblas::Layout::RowMajor,
                           cblas::Transpose::None,
                           cblas::Transpose::None,
                           m,
                           n,
                           1,
                           beta,
                           ones.data(),
                           1,
                           bias.data(),
                           std::max<size_t>(1, n),
                           1.0f,
                           matOut,
                           std::max<size_t>(1, ldc));
    }
    else
    {
        std::vector<float> identity(n * n, 0.0f);
        for (auto i = 0; i < n * n; i += n + 1)
        {
            identity[i] = 1.0;
        }
        cblas::cblas_sgemm(cblas::Layout::RowMajor,
                           cblas::Transpose::None,
                           cblas::Transpose::None,
                           m,
                           n,
                           n,
                           beta,
                           matC,
                           std::max<size_t>(1, n),
                           identity.data(),
                           std::max<size_t>(1, n),
                           1.0f,
                           matOut,
                           std::max<size_t>(1, ldc));
    }
}

extern "C" NGRAPH_API void __mlir_cblas_sgemm_scalar_bias(void* matAPtr,
                                                          void* matBPtr,
                                                          void* matCPtr,
                                                          void* matOutPtr,
                                                          const bool transposeA,
                                                          const bool transposeB,
                                                          const size_t m,
                                                          const size_t n,
                                                          const size_t k,
                                                          const size_t lda,
                                                          const size_t ldb,
                                                          const size_t ldc,
                                                          const float alpha,
                                                          const float beta,
                                                          const int broadcastHint)
{
    __mlir_cblas_sgemm_with_bias(matAPtr,
                                 matBPtr,
                                 matCPtr,
                                 matOutPtr,
                                 transposeA,
                                 transposeB,
                                 m,
                                 n,
                                 k,
                                 lda,
                                 ldb,
                                 ldc,
                                 alpha,
                                 beta,
                                 broadcastHint);
}

extern "C" NGRAPH_API void __mlir_cblas_sgemm_1d_bias(void* matAPtr,
                                                      void* matBPtr,
                                                      void* matCPtr,
                                                      void* matOutPtr,
                                                      const bool transposeA,
                                                      const bool transposeB,
                                                      const size_t m,
                                                      const size_t n,
                                                      const size_t k,
                                                      const size_t lda,
                                                      const size_t ldb,
                                                      const size_t ldc,
                                                      const float alpha,
                                                      const float beta,
                                                      const int broadcastHint)
{
    __mlir_cblas_sgemm_with_bias(matAPtr,
                                 matBPtr,
                                 matCPtr,
                                 matOutPtr,
                                 transposeA,
                                 transposeB,
                                 m,
                                 n,
                                 k,
                                 lda,
                                 ldb,
                                 ldc,
                                 alpha,
                                 beta,
                                 broadcastHint);
}

extern "C" NGRAPH_API void __mlir_cblas_sgemm_2d_bias(void* matAPtr,
                                                      void* matBPtr,
                                                      void* matCPtr,
                                                      void* matOutPtr,
                                                      const bool transposeA,
                                                      const bool transposeB,
                                                      const size_t m,
                                                      const size_t n,
                                                      const size_t k,
                                                      const size_t lda,
                                                      const size_t ldb,
                                                      const size_t ldc,
                                                      const float alpha,
                                                      const float beta,
                                                      const int broadcastHint)
{
    __mlir_cblas_sgemm_with_bias(matAPtr,
                                 matBPtr,
                                 matCPtr,
                                 matOutPtr,
                                 transposeA,
                                 transposeB,
                                 m,
                                 n,
                                 k,
                                 lda,
                                 ldb,
                                 ldc,
                                 alpha,
                                 beta,
                                 broadcastHint);
}

#define DEBUG_TYPE "mlir-cpu-runtime"

static llvm::cl::opt<bool>
    clDumpObjectFile("ngraph-dump-mlir-object-file",
                     llvm::cl::desc("Dump MLIR JITted-compiled object to file specified with "
                                    "-object-filename (<input file>.o by default)."));

static llvm::cl::opt<std::string>
    clObjectFilename("ngraph-mlir-object-filename",
                     llvm::cl::desc("Dump MLIR JITted-compiled object to file jitted_mlir.o"));

void MLIRCPURuntime::run(void* args,
                         const std::vector<std::vector<size_t>>& shapeVec,
                         const std::vector<std::vector<size_t>>& stridesVec)
{
    run_internal(*reinterpret_cast<std::vector<void*>*>(args), shapeVec, stridesVec);
}

void MLIRCPURuntime::run_internal(std::vector<void*>& externalTensors,
                                  const std::vector<std::vector<size_t>>& shapeVec,
                                  const std::vector<std::vector<size_t>>& stridesVec)
{
    // Create an MLIR execution engine. We use a null MLIR pass manager for now to make sure we
    // don't run MLIR passes that were already run. We also pass a default transformer created with
    // the default or user-provided optimization level.

    // m_module.get().dump();

    auto llvmTransformer = mlir::makeOptimizingTransformer(
        MLIRCPUBackend::mlirOptLevel, /*sizeLevel=*/0, MLIRCPUBackend::targetMachine.get());
    auto maybeEngine = mlir::ExecutionEngine::create(
        m_module.get(), llvmTransformer, MLIRCPUBackend::mlirOptLevel);
    NGRAPH_CHECK(maybeEngine, "failed to construct an execution engine");
    m_engine = std::move(maybeEngine.get());

    bindArguments(externalTensors, shapeVec, stridesVec);
    execute();
    cleanup();
}

// Binds MLIR function arguments to the proper values. This includes externally allocated tensors
// helpers to be used inside the function.
void MLIRCPURuntime::bindArguments(std::vector<void*>& externalTensors,
                                   const std::vector<std::vector<size_t>>& shapeVec,
                                   const std::vector<std::vector<size_t>>& stridesVec)
{
    NGRAPH_CHECK(m_module, "MLIR module is not ready.");

    mlir::FuncOp func = m_module->lookupSymbol<mlir::FuncOp>("main");
    NGRAPH_CHECK(func && !func.getBlocks().empty(), "Function not found");

    // Set external arguments
    m_externalTensors = &externalTensors;

    // Create list with a type-erased double pointer for each invocation arguments.
    // We currently use 'allocateMemrefArgs', which creates the arguments list per call ABI (see
    // comment below).
    // StaticFloatMemref is just a struct with the actual pointer to the data.

    for (auto i = 0; i < shapeVec.size(); i++)
    {
        m_ranks.push_back(shapeVec[i].size());
    }
    auto expectedArguments = allocateMemrefArgs();
    NGRAPH_CHECK(expectedArguments.size(), "Arguments can't be created");
    m_invokeArgs = std::move(expectedArguments);

    NGRAPH_CHECK(m_invokeArgs.size() == m_externalTensors->size(),
                 "Number of external tensors doesn't match number of function arguments");

    // Assign external tensor pointers to invocation arguments.
    for (size_t i = 0, numArgs = m_invokeArgs.size(); i < numArgs; ++i)
    {
        if (shapeVec[i].size() == 0)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<0>**>(m_invokeArgs[i]));
            memRefArg->basePtr = reinterpret_cast<float*>((*m_externalTensors)[i]);
        }
        else if (shapeVec[i].size() == 1)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<1>**>(m_invokeArgs[i]));
            memRefArg->basePtr = reinterpret_cast<float*>((*m_externalTensors)[i]);

            for (auto j = 0; j < 1; j++)
            {
                memRefArg->shape[j] = shapeVec[i][j];
                memRefArg->strides[j] = stridesVec[i][j];
            }
        }
        else if (shapeVec[i].size() == 2)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<2>**>(m_invokeArgs[i]));
            memRefArg->basePtr = reinterpret_cast<float*>((*m_externalTensors)[i]);

            for (auto j = 0; j < 2; j++)
            {
                memRefArg->shape[j] = shapeVec[i][j];
                memRefArg->strides[j] = stridesVec[i][j];
            }
        }
        if (shapeVec[i].size() == 3)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<3>**>(m_invokeArgs[i]));
            memRefArg->basePtr = reinterpret_cast<float*>((*m_externalTensors)[i]);

            for (auto j = 0; j < 3; j++)
            {
                memRefArg->shape[j] = shapeVec[i][j];
                memRefArg->strides[j] = stridesVec[i][j];
            }
        }
        if (shapeVec[i].size() == 4)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<4>**>(m_invokeArgs[i]));
            memRefArg->basePtr = reinterpret_cast<float*>((*m_externalTensors)[i]);

            for (auto j = 0; j < 4; j++)
            {
                memRefArg->shape[j] = shapeVec[i][j];
                memRefArg->strides[j] = stridesVec[i][j];
            }
        }
        if (shapeVec[i].size() == 5)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<5>**>(m_invokeArgs[i]));
            memRefArg->basePtr = reinterpret_cast<float*>((*m_externalTensors)[i]);

            for (auto j = 0; j < 5; j++)
            {
                memRefArg->shape[j] = shapeVec[i][j];
                memRefArg->strides[j] = stridesVec[i][j];
            }
        }
    }
}

// Lowers standard dialect to LLVM dialect and uses the MLIR execution engine to execute the code.
void MLIRCPURuntime::execute()
{
    // Invoke the JIT-compiled function with the arguments. Note that, for API
    // uniformity reasons, it takes a list of type-erased pointers to arguments.
    // Please, note that 'invoke' method is overloaded with a parameter pack version.
    // Make sure the MutableArrayRef version is invoked.
    auto invocationResult = m_engine->invoke("main", llvm::MutableArrayRef<void*>(m_invokeArgs));

    if (clDumpObjectFile)
    {
        m_engine->dumpToObjectFile(clObjectFilename.empty() ? "jitted_mlir.o"
                                                            : clObjectFilename.getValue());
    }
    NGRAPH_CHECK(!invocationResult, "JIT invocation of 'main' failed\n");
}

void MLIRCPURuntime::cleanup()
{
    // Free void double pointer arguments without freeing external tensor data.
    int i = 0;
    for (auto* arg : m_invokeArgs)
    {
        if (m_ranks[i] == 0)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<0>**>(arg));
            free(memRefArg);
        }
        else if (m_ranks[i] == 1)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<1>**>(arg));
            free(memRefArg);
        }
        else if (m_ranks[i] == 2)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<2>**>(arg));
            free(memRefArg);
        }
        else if (m_ranks[i] == 3)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<3>**>(arg));
            free(memRefArg);
        }
        else if (m_ranks[i] == 4)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<4>**>(arg));
            free(memRefArg);
        }
        else if (m_ranks[i] == 5)
        {
            auto* memRefArg = *(reinterpret_cast<StridedMemRef<5>**>(arg));
            free(memRefArg);
        }

        free(arg);
    }
}

// The current call ABI takes a single arg pointer (argPtr) pointing to a list of args.
// Each arg is a  pointer to a StaticFloatMemRef which contains a data pointer
//
// The args are laid out as follows
// argPtr-> arg[0]-> StaticFloatMemRef -> <data>
//          arg[1]-> StaticFloatMemRef -> <data>
//          ...
SmallVector<void*, 8> MLIRCPURuntime::allocateMemrefArgs()
{
    SmallVector<void*, 8> args;
    for (auto i = 0; i < m_externalTensors->size(); i++)
    {
        if (m_ranks[i] == 0)
        {
            std::cout << "malloc 0\n";
            auto descriptor = allocateMemrefDescriptor<0>();
            StridedMemRef<0>** arg =
                reinterpret_cast<StridedMemRef<0>**>(malloc(sizeof(StridedMemRef<0>*)));
            *arg = descriptor;
            args.push_back(arg);
        }
        else if (m_ranks[i] == 1)
        {
            std::cout << "malloc 1\n";
            auto descriptor = allocateMemrefDescriptor<1>();
            StridedMemRef<1>** arg =
                reinterpret_cast<StridedMemRef<1>**>(malloc(sizeof(StridedMemRef<1>*)));
            *arg = descriptor;
            args.push_back(arg);
        }
        else if (m_ranks[i] == 2)
        {
            std::cout << "malloc 2\n";
            auto descriptor = allocateMemrefDescriptor<2>();
            StridedMemRef<2>** arg =
                reinterpret_cast<StridedMemRef<2>**>(malloc(sizeof(StridedMemRef<2>*)));
            *arg = descriptor;
            args.push_back(arg);
        }
        else if (m_ranks[i] == 3)
        {
            std::cout << "malloc 3\n";
            auto descriptor = allocateMemrefDescriptor<3>();
            StridedMemRef<3>** arg =
                reinterpret_cast<StridedMemRef<3>**>(malloc(sizeof(StridedMemRef<3>*)));
            *arg = descriptor;
            args.push_back(arg);
        }
        else if (m_ranks[i] == 4)
        {
            std::cout << "malloc 4\n";
            auto descriptor = allocateMemrefDescriptor<4>();
            StridedMemRef<4>** arg =
                reinterpret_cast<StridedMemRef<4>**>(malloc(sizeof(StridedMemRef<4>*)));
            *arg = descriptor;
            args.push_back(arg);
        }
        else if (m_ranks[i] == 5)
        {
            std::cout << "malloc 5\n";
            auto descriptor = allocateMemrefDescriptor<5>();
            StridedMemRef<5>** arg =
                reinterpret_cast<StridedMemRef<5>**>(malloc(sizeof(StridedMemRef<5>*)));
            *arg = descriptor;
            args.push_back(arg);
        }
    }
    return args;
}

template <int N>
StridedMemRef<N>* MLIRCPURuntime::allocateMemrefDescriptor()
{
    // We only use StaticFloatMemRef because that's what MLIR currently offers.
    // We should expand this with different types and dynamic MemRefs
    auto* descriptor = reinterpret_cast<StridedMemRef<N>*>(malloc(sizeof(StridedMemRef<N>)));
    NGRAPH_CHECK(descriptor != nullptr, "NULL MemRef descriptor");
    descriptor->basePtr = nullptr;
    descriptor->offset = 0;
    return descriptor;
}
