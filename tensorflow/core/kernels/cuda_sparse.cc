/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifdef GOOGLE_CUDA

#include "tensorflow/core/kernels/cuda_sparse.h"

#include <complex>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "third_party/gpus/cuda/include/cusparse.h"
#include "tensorflow/core/common_runtime/gpu/gpu_event_mgr.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/kernels/cuda_solvers.h"
#include "tensorflow/core/lib/core/blocking_counter.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/stream_executor.h"
#include "tensorflow/core/platform/types.h"

// TODO(rmlarsen,penporn): Investigate using newer kernels in CUDA 10.1+.

namespace tensorflow {
namespace {

// Type traits to get CUDA complex types from std::complex<>.
// TODO: reuse with cuda_solvers
template <typename T>
struct CudaComplexT {
  typedef T type;
};
template <>
struct CudaComplexT<std::complex<float>> {
  typedef cuComplex type;
};
template <>
struct CudaComplexT<std::complex<double>> {
  typedef cuDoubleComplex type;
};
// Converts pointers of std::complex<> to pointers of
// cuComplex/cuDoubleComplex. No type conversion for non-complex types.
template <typename T>
inline const typename CudaComplexT<T>::type* AsCudaComplex(const T* p) {
  return reinterpret_cast<const typename CudaComplexT<T>::type*>(p);
}
template <typename T>
inline typename CudaComplexT<T>::type* AsCudaComplex(T* p) {
  return reinterpret_cast<typename CudaComplexT<T>::type*>(p);
}

// A set of initialized handles to the underlying Cuda libraries used by
// CudaSparse. We maintain one such set of handles per unique stream.
class CudaSparseHandles {
 public:
  explicit CudaSparseHandles(cudaStream_t stream)
      : initialized_(false), stream_(stream) {}

  CudaSparseHandles(CudaSparseHandles&& rhs)
      : initialized_(rhs.initialized_),
        stream_(std::move(rhs.stream_)),
        cusparse_handle_(rhs.cusparse_handle_) {
    rhs.initialized_ = false;
  }

  CudaSparseHandles& operator=(CudaSparseHandles&& rhs) {
    if (this == &rhs) return *this;
    Release();
    stream_ = std::move(rhs.stream_);
    cusparse_handle_ = std::move(rhs.cusparse_handle_);
    initialized_ = rhs.initialized_;
    rhs.initialized_ = false;
    return *this;
  }

  ~CudaSparseHandles() { Release(); }

  Status Initialize() {
    if (initialized_) return Status::OK();
    TF_RETURN_IF_CUSPARSE_ERROR(cusparseCreate(&cusparse_handle_));
    TF_RETURN_IF_CUSPARSE_ERROR(cusparseSetStream(cusparse_handle_, stream_));
    initialized_ = true;
    return Status::OK();
  }

  cusparseHandle_t& handle() {
    DCHECK(initialized_);
    return cusparse_handle_;
  }

  const cusparseHandle_t& handle() const {
    DCHECK(initialized_);
    return cusparse_handle_;
  }

 private:
  void Release() {
    if (initialized_) {
      // This should never return anything other than success
      auto err = cusparseDestroy(cusparse_handle_);
      DCHECK(err == CUSPARSE_STATUS_SUCCESS)
          << "Failed to destroy cuSparse instance.";
      initialized_ = false;
    }
  }
  bool initialized_;
  cudaStream_t stream_;
  cusparseHandle_t cusparse_handle_;

  TF_DISALLOW_COPY_AND_ASSIGN(CudaSparseHandles);
};

// TODO(ebrevdo): Replace global mutex guarding CudaSparseHandles
// lookup with one of:
//    1. Adding the handle to the CudaStream structure; do the lookup there.
//    2. Add a thread-local cusparse, set it to the current stream
//       upon each call.
// #1 seems like the cleanest option but will need to wait until this
// is moved into TF core.
static mutex handle_map_mutex(LINKER_INITIALIZED);

using HandleMap = std::unordered_map<cudaStream_t, CudaSparseHandles>;

// Returns a singleton map used for storing initialized handles for each unique
// cuda stream.
HandleMap* GetHandleMapSingleton() {
  static HandleMap* cm = new HandleMap;
  return cm;
}

}  // namespace

CudaSparse::CudaSparse(OpKernelContext* context)
    : initialized_(false), context_(context) {
  auto cuda_stream_ptr =
      reinterpret_cast<const cudaStream_t*>(context->op_device_context()
                                                ->stream()
                                                ->implementation()
                                                ->GpuStreamMemberHack());
  DCHECK(cuda_stream_ptr);
  cuda_stream_ = *cuda_stream_ptr;
}

Status CudaSparse::Initialize() {
  HandleMap* handle_map = GetHandleMapSingleton();
  DCHECK(handle_map);
  mutex_lock lock(handle_map_mutex);
  auto it = handle_map->find(cuda_stream_);
  if (it == handle_map->end()) {
    LOG(INFO) << "Creating CudaSparse handles for stream " << cuda_stream_;
    // Previously unseen Cuda stream. Initialize a set of Cuda sparse library
    // handles for it.
    CudaSparseHandles new_handles(cuda_stream_);
    TF_RETURN_IF_ERROR(new_handles.Initialize());
    it =
        handle_map->insert(std::make_pair(cuda_stream_, std::move(new_handles)))
            .first;
  }
  cusparse_handle_ = &it->second.handle();
  initialized_ = true;
  return Status::OK();
}

// Macro that specializes a sparse method for all 4 standard
// numeric types.
// TODO: reuse with cuda_solvers
#define TF_CALL_LAPACK_TYPES(m) \
  m(float, S) m(double, D) m(std::complex<float>, C) m(std::complex<double>, Z)

// Macros to construct cusparse method names.
#define SPARSE_FN(method, sparse_prefix) cusparse##sparse_prefix##method
#define SPARSE_NAME(method, sparse_prefix) "cusparse" #sparse_prefix #method
#define BUFSIZE_FN(method, sparse_prefix) \
  cusparse##sparse_prefix##method##_bufferSizeExt

//=============================================================================
// Wrappers of cuSparse computational methods begin here.
//
// WARNING to implementers: The function signatures listed in the online docs
// are sometimes inaccurate, e.g., are missing 'const' on pointers
// to immutable arguments, while the actual headers have them as expected.
// Check the actual declarations in the cusparse.h header file.
//=============================================================================

template <typename Scalar, typename SparseFn>
static inline Status GtsvImpl(SparseFn op, cusparseHandle_t cusparse_handle,
                              int m, int n, const Scalar* dl, const Scalar* d,
                              const Scalar* du, Scalar* B, int ldb) {
  TF_RETURN_IF_CUSPARSE_ERROR(op(cusparse_handle, m, n, AsCudaComplex(dl),
                                 AsCudaComplex(d), AsCudaComplex(du),
                                 AsCudaComplex(B), ldb));
  return Status::OK();
}

#define GTSV_INSTANCE(Scalar, sparse_prefix)                                 \
  template <>                                                                \
  Status CudaSparse::Gtsv<Scalar>(int m, int n, const Scalar* dl,            \
                                  const Scalar* d, const Scalar* du,         \
                                  Scalar* B, int ldb) const {                \
    DCHECK(initialized_);                                                    \
    return GtsvImpl(SPARSE_FN(gtsv, sparse_prefix), *cusparse_handle_, m, n, \
                    dl, d, du, B, ldb);                                      \
  }

TF_CALL_LAPACK_TYPES(GTSV_INSTANCE);

#define GTSV_NO_PIVOT_INSTANCE(Scalar, sparse_prefix)                          \
  template <>                                                                  \
  Status CudaSparse::GtsvNoPivot<Scalar>(int m, int n, const Scalar* dl,       \
                                         const Scalar* d, const Scalar* du,    \
                                         Scalar* B, int ldb) const {           \
    DCHECK(initialized_);                                                      \
    return GtsvImpl(SPARSE_FN(gtsv_nopivot, sparse_prefix), *cusparse_handle_, \
                    m, n, dl, d, du, B, ldb);                                  \
  }

TF_CALL_LAPACK_TYPES(GTSV_NO_PIVOT_INSTANCE);

template <typename Scalar, typename SparseFn>
static inline Status GtsvStridedBatchImpl(SparseFn op,
                                          cusparseHandle_t cusparse_handle,
                                          int m, const Scalar* dl,
                                          const Scalar* d, const Scalar* du,
                                          Scalar* x, int batchCount,
                                          int batchStride) {
  TF_RETURN_IF_CUSPARSE_ERROR(op(cusparse_handle, m, AsCudaComplex(dl),
                                 AsCudaComplex(d), AsCudaComplex(du),
                                 AsCudaComplex(x), batchCount, batchStride));
  return Status::OK();
}

#define GTSV_STRIDED_BATCH_INSTANCE(Scalar, sparse_prefix)                   \
  template <>                                                                \
  Status CudaSparse::GtsvStridedBatch<Scalar>(                               \
      int m, const Scalar* dl, const Scalar* d, const Scalar* du, Scalar* x, \
      int batchCount, int batchStride) const {                               \
    DCHECK(initialized_);                                                    \
    return GtsvStridedBatchImpl(SPARSE_FN(gtsvStridedBatch, sparse_prefix),  \
                                *cusparse_handle_, m, dl, d, du, x,          \
                                batchCount, batchStride);                    \
  }

TF_CALL_LAPACK_TYPES(GTSV_STRIDED_BATCH_INSTANCE);

template <typename Scalar, typename SparseFn>
static inline Status Gtsv2Impl(SparseFn op, cusparseHandle_t cusparse_handle,
                               int m, int n, const Scalar* dl, const Scalar* d,
                               const Scalar* du, Scalar* B, int ldb,
                               void* pBuffer) {
  TF_RETURN_IF_CUSPARSE_ERROR(op(cusparse_handle, m, n, AsCudaComplex(dl),
                                 AsCudaComplex(d), AsCudaComplex(du),
                                 AsCudaComplex(B), ldb, pBuffer));
  return Status::OK();
}

#define GTSV2_INSTANCE(Scalar, sparse_prefix)                                  \
  template <>                                                                  \
  Status CudaSparse::Gtsv2<Scalar>(int m, int n, const Scalar* dl,             \
                                   const Scalar* d, const Scalar* du,          \
                                   Scalar* B, int ldb, void* pBuffer) const {  \
    DCHECK(initialized_);                                                      \
    return Gtsv2Impl(SPARSE_FN(gtsv2, sparse_prefix), *cusparse_handle_, m, n, \
                     dl, d, du, B, ldb, pBuffer);                              \
  }

TF_CALL_LAPACK_TYPES(GTSV2_INSTANCE);

#define GTSV2_NO_PIVOT_INSTANCE(Scalar, sparse_prefix)                     \
  template <>                                                              \
  Status CudaSparse::Gtsv2NoPivot<Scalar>(                                 \
      int m, int n, const Scalar* dl, const Scalar* d, const Scalar* du,   \
      Scalar* B, int ldb, void* pBuffer) const {                           \
    DCHECK(initialized_);                                                  \
    return Gtsv2Impl(SPARSE_FN(gtsv2_nopivot, sparse_prefix),              \
                     *cusparse_handle_, m, n, dl, d, du, B, ldb, pBuffer); \
  }

TF_CALL_LAPACK_TYPES(GTSV2_NO_PIVOT_INSTANCE);

template <typename Scalar, typename SparseFn>
static inline Status Gtsv2BufferSizeExtImpl(SparseFn op,
                                            cusparseHandle_t cusparse_handle,
                                            int m, int n, const Scalar* dl,
                                            const Scalar* d, const Scalar* du,
                                            const Scalar* B, int ldb,
                                            size_t* bufferSizeInBytes) {
  TF_RETURN_IF_CUSPARSE_ERROR(op(cusparse_handle, m, n, AsCudaComplex(dl),
                                 AsCudaComplex(d), AsCudaComplex(du),
                                 AsCudaComplex(B), ldb, bufferSizeInBytes));
  return Status::OK();
}

#define GTSV2_BUFFER_SIZE_INSTANCE(Scalar, sparse_prefix)                    \
  template <>                                                                \
  Status CudaSparse::Gtsv2BufferSizeExt<Scalar>(                             \
      int m, int n, const Scalar* dl, const Scalar* d, const Scalar* du,     \
      const Scalar* B, int ldb, size_t* bufferSizeInBytes) const {           \
    DCHECK(initialized_);                                                    \
    return Gtsv2BufferSizeExtImpl(                                           \
        SPARSE_FN(gtsv2_bufferSizeExt, sparse_prefix), *cusparse_handle_, m, \
        n, dl, d, du, B, ldb, bufferSizeInBytes);                            \
  }

TF_CALL_LAPACK_TYPES(GTSV2_BUFFER_SIZE_INSTANCE);

#define GTSV2_NO_PIVOT_BUFFER_SIZE_INSTANCE(Scalar, sparse_prefix)       \
  template <>                                                            \
  Status CudaSparse::Gtsv2NoPivotBufferSizeExt<Scalar>(                  \
      int m, int n, const Scalar* dl, const Scalar* d, const Scalar* du, \
      const Scalar* B, int ldb, size_t* bufferSizeInBytes) const {       \
    DCHECK(initialized_);                                                \
    return Gtsv2BufferSizeExtImpl(                                       \
        SPARSE_FN(gtsv2_nopivot_bufferSizeExt, sparse_prefix),           \
        *cusparse_handle_, m, n, dl, d, du, B, ldb, bufferSizeInBytes);  \
  }

TF_CALL_LAPACK_TYPES(GTSV2_NO_PIVOT_BUFFER_SIZE_INSTANCE);

template <typename Scalar, typename SparseFn>
static inline Status Gtsv2StridedBatchImpl(SparseFn op,
                                           cusparseHandle_t cusparse_handle,
                                           int m, const Scalar* dl,
                                           const Scalar* d, const Scalar* du,
                                           Scalar* x, int batchCount,
                                           int batchStride, void* pBuffer) {
  TF_RETURN_IF_CUSPARSE_ERROR(op(
      cusparse_handle, m, AsCudaComplex(dl), AsCudaComplex(d),
      AsCudaComplex(du), AsCudaComplex(x), batchCount, batchStride, pBuffer));
  return Status::OK();
}

#define GTSV2_STRIDED_BATCH_INSTANCE(Scalar, sparse_prefix)                   \
  template <>                                                                 \
  Status CudaSparse::Gtsv2StridedBatch<Scalar>(                               \
      int m, const Scalar* dl, const Scalar* d, const Scalar* du, Scalar* x,  \
      int batchCount, int batchStride, void* pBuffer) const {                 \
    DCHECK(initialized_);                                                     \
    return Gtsv2StridedBatchImpl(SPARSE_FN(gtsv2StridedBatch, sparse_prefix), \
                                 *cusparse_handle_, m, dl, d, du, x,          \
                                 batchCount, batchStride, pBuffer);           \
  }

TF_CALL_LAPACK_TYPES(GTSV2_STRIDED_BATCH_INSTANCE);

template <typename Scalar, typename SparseFn>
static inline Status Gtsv2StridedBatchBufferSizeImpl(
    SparseFn op, cusparseHandle_t cusparse_handle, int m, const Scalar* dl,
    const Scalar* d, const Scalar* du, const Scalar* x, int batchCount,
    int batchStride, size_t* bufferSizeInBytes) {
  TF_RETURN_IF_CUSPARSE_ERROR(op(cusparse_handle, m, AsCudaComplex(dl),
                                 AsCudaComplex(d), AsCudaComplex(du),
                                 AsCudaComplex(x), batchCount, batchStride,
                                 bufferSizeInBytes));
  return Status::OK();
}

#define GTSV2_STRIDED_BATCH_BUFFER_SIZE_INSTANCE(Scalar, sparse_prefix) \
  template <>                                                           \
  Status CudaSparse::Gtsv2StridedBatchBufferSizeExt<Scalar>(            \
      int m, const Scalar* dl, const Scalar* d, const Scalar* du,       \
      const Scalar* x, int batchCount, int batchStride,                 \
      size_t* bufferSizeInBytes) const {                                \
    DCHECK(initialized_);                                               \
    return Gtsv2StridedBatchBufferSizeImpl(                             \
        SPARSE_FN(gtsv2StridedBatch_bufferSizeExt, sparse_prefix),      \
        *cusparse_handle_, m, dl, d, du, x, batchCount, batchStride,    \
        bufferSizeInBytes);                                             \
  }

TF_CALL_LAPACK_TYPES(GTSV2_STRIDED_BATCH_BUFFER_SIZE_INSTANCE);

Status CudaSparse::Coo2csr(const int* cooRowInd, int nnz, int m,
                           int* csrRowPtr) const {
  // cusparseStatus_t CUSPARSEAPI cusparseXcoo2csr(cusparseHandle_t handle,
  //                                               const int *cooRowInd,
  //                                               int nnz,
  //                                               int m,
  //                                               int *csrSortedRowPtr,
  //                                               cusparseIndexBase_t
  //                                               idxBase);
  DCHECK(initialized_);
  TF_RETURN_IF_CUSPARSE_ERROR(cusparseXcoo2csr(*cusparse_handle_, cooRowInd,
                                               nnz, m, csrRowPtr,
                                               CUSPARSE_INDEX_BASE_ZERO));
  return Status::OK();
}

Status CudaSparse::Csr2coo(const int* csrRowPtr, int nnz, int m,
                           int* cooRowInd) const {
  // cusparseStatus_t CUSPARSEAPI cusparseXcsr2coo(cusparseHandle_t handle,
  //                                               const int *csrRowPtr,
  //                                               int nnz,
  //                                               int m,
  //                                               int *cooRowInd,
  //                                               cusparseIndexBase_t
  //                                               idxBase);
  DCHECK(initialized_);
  TF_RETURN_IF_CUSPARSE_ERROR(cusparseXcsr2coo(*cusparse_handle_, csrRowPtr,
                                               nnz, m, cooRowInd,
                                               CUSPARSE_INDEX_BASE_ZERO));
  return Status::OK();
}

Status CudaSparse::CsrgeamNnz(int m, int n, const cusparseMatDescr_t descrA,
                              int nnzA, const int* csrSortedRowPtrA,
                              const int* csrSortedColIndA,
                              const cusparseMatDescr_t descrB, int nnzB,
                              const int* csrSortedRowPtrB,
                              const int* csrSortedColIndB,
                              const cusparseMatDescr_t descrC,
                              int* csrSortedRowPtrC, int* nnzTotalDevHostPtr) {
  DCHECK(initialized_);
  DCHECK(nnzTotalDevHostPtr != nullptr);
  TF_RETURN_IF_CUSPARSE_ERROR(cusparseXcsrgeamNnz(
      *cusparse_handle_, m, n, descrA, nnzA, csrSortedRowPtrA, csrSortedColIndA,
      descrB, nnzB, csrSortedRowPtrB, csrSortedColIndB, descrC,
      csrSortedRowPtrC, nnzTotalDevHostPtr));
  return Status::OK();
}

template <typename Scalar, typename SparseFnT>
static inline Status CsrmmImpl(
    SparseFnT op, OpKernelContext* context, cusparseHandle_t cusparse_handle,
    cusparseOperation_t transA, cusparseOperation_t transB, int m, int n, int k,
    int nnz, const Scalar* alpha_host, const cusparseMatDescr_t descrA,
    const Scalar* csrSortedValA, const int* csrSortedRowPtrA,
    const int* csrSortedColIndA, const Scalar* B, int ldb,
    const Scalar* beta_host, Scalar* C, int ldc) {
  // cusparseStatus_t CUSPARSEAPI cusparseScsrmm2(
  //     cusparseHandle_t handle, cusparseOperation_t transA,
  //     cusparseOperation_t transB, int m, int n, int k, int nnz,
  //     const float* alpha, const cusparseMatDescr_t descrA,
  //     const float* csrSortedValA, const int* csrSortedRowPtrA,
  //     const int* csrSortedColIndA, const float* B, int ldb, const float*
  //     beta, float* C, int ldc);
  TF_RETURN_IF_CUSPARSE_ERROR(op(
      cusparse_handle, transA, transB, m, n, k, nnz, AsCudaComplex(alpha_host),
      descrA, AsCudaComplex(csrSortedValA), csrSortedRowPtrA, csrSortedColIndA,
      AsCudaComplex(B), ldb, AsCudaComplex(beta_host), AsCudaComplex(C), ldc));
  return Status::OK();
}

#define CSRMM_INSTANCE(Scalar, sparse_prefix)                                \
  template <>                                                                \
  Status CudaSparse::Csrmm<Scalar>(                                          \
      cusparseOperation_t transA, cusparseOperation_t transB, int m, int n,  \
      int k, int nnz, const Scalar* alpha_host,                              \
      const cusparseMatDescr_t descrA, const Scalar* csrSortedValA,          \
      const int* csrSortedRowPtrA, const int* csrSortedColIndA,              \
      const Scalar* B, int ldb, const Scalar* beta_host, Scalar* C, int ldc) \
      const {                                                                \
    DCHECK(initialized_);                                                    \
    return CsrmmImpl(SPARSE_FN(csrmm2, sparse_prefix), context_,             \
                     *cusparse_handle_, transA, transB, m, n, k, nnz,        \
                     alpha_host, descrA, csrSortedValA, csrSortedRowPtrA,    \
                     csrSortedColIndA, B, ldb, beta_host, C, ldc);           \
  }

TF_CALL_LAPACK_TYPES(CSRMM_INSTANCE);

template <typename Scalar, typename SparseFnT>
static inline Status CsrmvImpl(
    SparseFnT op, OpKernelContext* context, cusparseHandle_t cusparse_handle,
    cusparseOperation_t transA, int m, int n, int nnz, const Scalar* alpha_host,
    const cusparseMatDescr_t descrA, const Scalar* csrSortedValA,
    const int* csrSortedRowPtrA, const int* csrSortedColIndA, const Scalar* x,
    const Scalar* beta_host, Scalar* y) {
  TF_RETURN_IF_CUSPARSE_ERROR(
      op(cusparse_handle, transA, m, n, nnz, AsCudaComplex(alpha_host), descrA,
         AsCudaComplex(csrSortedValA), csrSortedRowPtrA, csrSortedColIndA,
         AsCudaComplex(x), AsCudaComplex(beta_host), AsCudaComplex(y)));
  return Status::OK();
}

// TODO(ebrevdo,rmlarsen): Use csrmv_mp for all cases when available in CUDA 9.
#define CSRMV_INSTANCE(Scalar, sparse_prefix)                                \
  template <>                                                                \
  Status CudaSparse::Csrmv<Scalar>(                                          \
      cusparseOperation_t transA, int m, int n, int nnz,                     \
      const Scalar* alpha_host, const cusparseMatDescr_t descrA,             \
      const Scalar* csrSortedValA, const int* csrSortedRowPtrA,              \
      const int* csrSortedColIndA, const Scalar* x, const Scalar* beta_host, \
      Scalar* y) const {                                                     \
    DCHECK(initialized_);                                                    \
    if (transA == CUSPARSE_OPERATION_NON_TRANSPOSE) {                        \
      return CsrmvImpl(SPARSE_FN(csrmv_mp, sparse_prefix), context_,         \
                       *cusparse_handle_, transA, m, n, nnz, alpha_host,     \
                       descrA, csrSortedValA, csrSortedRowPtrA,              \
                       csrSortedColIndA, x, beta_host, y);                   \
    } else {                                                                 \
      return CsrmvImpl(SPARSE_FN(csrmv, sparse_prefix), context_,            \
                       *cusparse_handle_, transA, m, n, nnz, alpha_host,     \
                       descrA, csrSortedValA, csrSortedRowPtrA,              \
                       csrSortedColIndA, x, beta_host, y);                   \
    }                                                                        \
  }

TF_CALL_LAPACK_TYPES(CSRMV_INSTANCE);

template <typename Scalar, typename SparseFnT>
static inline Status CsrgeamImpl(
    SparseFnT op, OpKernelContext* context, cusparseHandle_t cusparse_handle,
    int m, int n, const Scalar* alpha, const cusparseMatDescr_t descrA,
    int nnzA, const Scalar* csrSortedValA, const int* csrSortedRowPtrA,
    const int* csrSortedColIndA, const Scalar* beta,
    const cusparseMatDescr_t descrB, int nnzB, const Scalar* csrSortedValB,
    const int* csrSortedRowPtrB, const int* csrSortedColIndB,
    const cusparseMatDescr_t descrC, Scalar* csrSortedValC,
    int* csrSortedRowPtrC, int* csrSortedColIndC) {
  TF_RETURN_IF_CUSPARSE_ERROR(
      op(cusparse_handle, m, n, AsCudaComplex(alpha), descrA, nnzA,
         AsCudaComplex(csrSortedValA), csrSortedRowPtrA, csrSortedColIndA,
         AsCudaComplex(beta), descrB, nnzB, AsCudaComplex(csrSortedValB),
         csrSortedRowPtrB, csrSortedColIndB, descrC,
         AsCudaComplex(csrSortedValC), csrSortedRowPtrC, csrSortedColIndC));
  return Status::OK();
}

#define CSRGEAM_INSTANCE(Scalar, sparse_prefix)                               \
  template <>                                                                 \
  Status CudaSparse::Csrgeam<Scalar>(                                         \
      int m, int n, const Scalar* alpha, const cusparseMatDescr_t descrA,     \
      int nnzA, const Scalar* csrSortedValA, const int* csrSortedRowPtrA,     \
      const int* csrSortedColIndA, const Scalar* beta,                        \
      const cusparseMatDescr_t descrB, int nnzB, const Scalar* csrSortedValB, \
      const int* csrSortedRowPtrB, const int* csrSortedColIndB,               \
      const cusparseMatDescr_t descrC, Scalar* csrSortedValC,                 \
      int* csrSortedRowPtrC, int* csrSortedColIndC) {                         \
    DCHECK(initialized_);                                                     \
    return CsrgeamImpl(SPARSE_FN(csrgeam, sparse_prefix), context_,           \
                       *cusparse_handle_, m, n, alpha, descrA, nnzA,          \
                       csrSortedValA, csrSortedRowPtrA, csrSortedColIndA,     \
                       beta, descrB, nnzB, csrSortedValB, csrSortedRowPtrB,   \
                       csrSortedColIndB, descrC, csrSortedValC,               \
                       csrSortedRowPtrC, csrSortedColIndC);                   \
  }

TF_CALL_LAPACK_TYPES(CSRGEAM_INSTANCE);

Status CudaSparse::CsrgemmNnz(
    cusparseOperation_t transA, cusparseOperation_t transB, int m, int k, int n,
    const cusparseMatDescr_t descrA, int nnzA, const int* csrSortedRowPtrA,
    const int* csrSortedColIndA, const cusparseMatDescr_t descrB, int nnzB,
    const int* csrSortedRowPtrB, const int* csrSortedColIndB,
    const cusparseMatDescr_t descrC, int* csrSortedRowPtrC,
    int* nnzTotalDevHostPtr) {
  DCHECK(initialized_);
  DCHECK(nnzTotalDevHostPtr != nullptr);
  TF_RETURN_IF_CUSPARSE_ERROR(cusparseXcsrgemmNnz(
      *cusparse_handle_, transA, transB, m, k, n, descrA, nnzA,
      csrSortedRowPtrA, csrSortedColIndA, descrB, nnzB, csrSortedRowPtrB,
      csrSortedColIndB, descrC, csrSortedRowPtrC, nnzTotalDevHostPtr));
  return Status::OK();
}

template <typename Scalar, typename SparseFnT>
static inline Status CsrgemmImpl(
    SparseFnT op, OpKernelContext* context, cusparseHandle_t cusparse_handle,
    cusparseOperation_t transA, cusparseOperation_t transB, int m, int k, int n,
    const cusparseMatDescr_t descrA, int nnzA, const Scalar* csrSortedValA,
    const int* csrSortedRowPtrA, const int* csrSortedColIndA,
    const cusparseMatDescr_t descrB, int nnzB, const Scalar* csrSortedValB,
    const int* csrSortedRowPtrB, const int* csrSortedColIndB,
    const cusparseMatDescr_t descrC, Scalar* csrSortedValC,
    int* csrSortedRowPtrC, int* csrSortedColIndC) {
  TF_RETURN_IF_CUSPARSE_ERROR(
      op(cusparse_handle, transA, transB, m, k, n, descrA, nnzA,
         AsCudaComplex(csrSortedValA), csrSortedRowPtrA, csrSortedColIndA,
         descrB, nnzB, AsCudaComplex(csrSortedValB), csrSortedRowPtrB,
         csrSortedColIndB, descrC, AsCudaComplex(csrSortedValC),
         csrSortedRowPtrC, csrSortedColIndC));
  return Status::OK();
}

#define CSRGEMM_INSTANCE(Scalar, sparse_prefix)                               \
  template <>                                                                 \
  Status CudaSparse::Csrgemm<Scalar>(                                         \
      cusparseOperation_t transA, cusparseOperation_t transB, int m, int k,   \
      int n, const cusparseMatDescr_t descrA, int nnzA,                       \
      const Scalar* csrSortedValA, const int* csrSortedRowPtrA,               \
      const int* csrSortedColIndA, const cusparseMatDescr_t descrB, int nnzB, \
      const Scalar* csrSortedValB, const int* csrSortedRowPtrB,               \
      const int* csrSortedColIndB, const cusparseMatDescr_t descrC,           \
      Scalar* csrSortedValC, int* csrSortedRowPtrC, int* csrSortedColIndC) {  \
    DCHECK(initialized_);                                                     \
    return CsrgemmImpl(SPARSE_FN(csrgemm, sparse_prefix), context_,           \
                       *cusparse_handle_, transA, transB, m, k, n, descrA,    \
                       nnzA, csrSortedValA, csrSortedRowPtrA,                 \
                       csrSortedColIndA, descrB, nnzB, csrSortedValB,         \
                       csrSortedRowPtrB, csrSortedColIndB, descrC,            \
                       csrSortedValC, csrSortedRowPtrC, csrSortedColIndC);    \
  }

TF_CALL_LAPACK_TYPES(CSRGEMM_INSTANCE);

template <typename Scalar, typename BufferSizeFnT, typename SparseFnT>
static inline Status Csru2csrImpl(SparseFnT op, BufferSizeFnT buffer_size_op,
                                  OpKernelContext* context,
                                  cusparseHandle_t cusparse_handle, int m,
                                  int n, int nnz,
                                  const cusparseMatDescr_t descrA,
                                  Scalar* csrVal, const int* csrRowPtr,
                                  int* csrColInd) {
  CudaSparseCsrSortingConversionInfo info;
  TF_RETURN_IF_ERROR(info.Initialize());

  size_t pBufferSizeInBytes = 0;

  TF_RETURN_IF_CUSPARSE_ERROR(
      buffer_size_op(cusparse_handle, m, n, nnz, AsCudaComplex(csrVal),
                     csrRowPtr, csrColInd, info.info(), &pBufferSizeInBytes));

  Tensor pBuffer_t;
  TF_RETURN_IF_ERROR(context->allocate_temp(
      DT_INT8, TensorShape({static_cast<int64>(pBufferSizeInBytes)}),
      &pBuffer_t));
  auto pBuffer = pBuffer_t.flat<int8>();
  DCHECK(pBuffer.data() != nullptr);

  TF_RETURN_IF_CUSPARSE_ERROR(op(cusparse_handle, m, n, nnz, descrA,
                                 AsCudaComplex(csrVal), csrRowPtr, csrColInd,
                                 info.info(), pBuffer.data()));

  return Status::OK();
}

#define CSRU2CSR_INSTANCE(Scalar, sparse_prefix)                              \
  template <>                                                                 \
  Status CudaSparse::Csru2csr<Scalar>(                                        \
      int m, int n, int nnz, const cusparseMatDescr_t descrA, Scalar* csrVal, \
      const int* csrRowPtr, int* csrColInd) {                                 \
    DCHECK(initialized_);                                                     \
    return Csru2csrImpl(SPARSE_FN(csru2csr, sparse_prefix),                   \
                        BUFSIZE_FN(csru2csr, sparse_prefix), context_,        \
                        *cusparse_handle_, m, n, nnz, descrA, csrVal,         \
                        csrRowPtr, csrColInd);                                \
  }

TF_CALL_LAPACK_TYPES(CSRU2CSR_INSTANCE);

template <typename Scalar, typename SparseFnT>
static inline Status Csr2cscImpl(SparseFnT op, OpKernelContext* context,
                                 cusparseHandle_t cusparse_handle, int m, int n,
                                 int nnz, const Scalar* csrVal,
                                 const int* csrRowPtr, const int* csrColInd,
                                 Scalar* cscVal, int* cscRowInd, int* cscColPtr,
                                 const cusparseAction_t copyValues) {
  TF_RETURN_IF_CUSPARSE_ERROR(op(cusparse_handle, m, n, nnz,
                                 AsCudaComplex(csrVal), csrRowPtr, csrColInd,
                                 AsCudaComplex(cscVal), cscRowInd, cscColPtr,
                                 copyValues, CUSPARSE_INDEX_BASE_ZERO));
  return Status::OK();
}

#define CSR2CSC_INSTANCE(Scalar, sparse_prefix)                              \
  template <>                                                                \
  Status CudaSparse::Csr2csc<Scalar>(                                        \
      int m, int n, int nnz, const Scalar* csrVal, const int* csrRowPtr,     \
      const int* csrColInd, Scalar* cscVal, int* cscRowInd, int* cscColPtr,  \
      const cusparseAction_t copyValues) {                                   \
    DCHECK(initialized_);                                                    \
    return Csr2cscImpl(SPARSE_FN(csr2csc, sparse_prefix), context_,          \
                       *cusparse_handle_, m, n, nnz, csrVal, csrRowPtr,      \
                       csrColInd, cscVal, cscRowInd, cscColPtr, copyValues); \
  }

TF_CALL_LAPACK_TYPES(CSR2CSC_INSTANCE);

}  // namespace tensorflow

#endif  // GOOGLE_CUDA
