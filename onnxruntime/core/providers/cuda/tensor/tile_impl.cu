// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cuda/cu_inc/common.cuh"
#include "tile_impl.h"

namespace onnxruntime {
namespace cuda {

constexpr int MAX_DIMS = 16;

template <typename T>
__global__ void _UnRolledTileKernel(
    const size_t shape_rank,
    const TArray<fast_divmod> fdm_input_shape,
    const TArray<int64_t> input_strides,
    const T* input_data,
    const TArray<fast_divmod> fdm_output_strides,
    T* output_data,
    const CUDA_LONG N) {
  CALCULATE_ELEMENTWISE_INDEX_OR_EXIT(id, N);
  CUDA_LONG input_index = 0;
  CUDA_LONG output_index = id;

  #pragma unroll
  for (int dim = 0; dim < MAX_DIMS; ++dim) {
    if (dim == shape_rank) {
      break;
    }
    int out_coord, r;
    fdm_output_strides[dim].divmod(output_index, out_coord, r);
    output_index = r;
    int in_coord = fdm_input_shape[dim].mod(out_coord);
    input_index += input_strides[dim] * in_coord;
  }
  output_data[id] = input_data[input_index];
}

template <typename T>
__global__ void _TileKernel(
    const size_t shape_rank,
    const TArray<fast_divmod> fdm_input_shape,
    const TArray<int64_t> input_strides,
    const T* input_data,
    const TArray<fast_divmod> fdm_output_strides,
    T* output_data,
    const CUDA_LONG N) {
  CALCULATE_ELEMENTWISE_INDEX_OR_EXIT(id, N);
  CUDA_LONG input_index = 0;
  CUDA_LONG output_index = id;
  for (int dim = 0; dim < shape_rank; ++dim) {
    int out_coord, r;
    fdm_output_strides[dim].divmod(output_index, out_coord, r);
    output_index = r;
    int in_coord = fdm_input_shape[dim].mod(out_coord);
    input_index += input_strides[dim] * in_coord;
  }
  output_data[id] = input_data[input_index];
}

template <typename T>
void TileImpl(
    cudaStream_t stream,
    const size_t shape_rank,
    const TArray<fast_divmod>& fdm_input_shape,
    const TArray<int64_t>& input_stride,
    const T* input_data,
    const TArray<fast_divmod>& fdm_output_strides,
    T* output_data,
    const size_t N) {
  int blocksPerGrid = (int)(ceil(static_cast<float>(N) / GridDim::maxThreadsPerBlock));
  if (shape_rank > MAX_DIMS) {
    _TileKernel<T><<<blocksPerGrid, GridDim::maxThreadsPerBlock, 0, stream>>>(
        shape_rank, fdm_input_shape, input_stride, input_data,
        fdm_output_strides, output_data, (CUDA_LONG)N);
  } else {
    _UnRolledTileKernel<T><<<blocksPerGrid, GridDim::maxThreadsPerBlock, 0, stream>>>(
        shape_rank, fdm_input_shape, input_stride, input_data,
        fdm_output_strides, output_data, (CUDA_LONG)N);
  }
}

template <typename T>
__global__ void _TileMemcpyKernel(
    const T* input_data,
    const size_t num_input_elements,
    T* output_data,
    const size_t N) {
  CALCULATE_ELEMENTWISE_INDEX_OR_EXIT(id, N);
  auto input_index = id % num_input_elements;
  output_data[id] = input_data[input_index];
}

template <typename T>
__global__ void _TileMemcpyKernel_opt(
    const T* input_data,
    const size_t num_input_elements,
    T* output_data,
    const size_t N,
    const size_t num_per_thread) {
  CALCULATE_ELEMENTWISE_INDEX_OR_EXIT(index_of_input, num_input_elements)
  auto input_val = input_data[index_of_input];
  for (size_t i = 0; i < num_per_thread; i++) {
    auto index_of_output = index_of_input + num_input_elements * i;
    output_data[index_of_output] = input_val;
  }
}

template <typename T>
void TileMemcpyImpl(
    cudaStream_t stream,
    const T* input_data,
    const size_t num_input_elements,
    T* output_data,
    const size_t num_output_elements) {
  auto can_be_divided = ((num_output_elements / num_input_elements) * num_input_elements) == num_output_elements;
  ORT_ENFORCE(can_be_divided,
              "output shape must be multiple times of input shape,"
              "while output shape is ",
              num_output_elements, " and input shape is ", num_input_elements);
  // each block has 256 threads, total block will be ceil(num_input / 256) >> namely, each thread takes care one input element
  // each cuda thread processes num_output/num_input output data elements
  int blocks = (int)(ceil(static_cast<float>(num_input_elements) / GridDim::maxThreadsPerBlock));
  auto num_per_thread = num_output_elements / num_input_elements;
  // GPU SMs is 100 around, so 128 should be enough blocks to occupy all SMs
  if (blocks >= 128) {
    std::cout << "used\n\n\n";
    _TileMemcpyKernel_opt<<<blocks, GridDim::maxThreadsPerBlock, 0, stream>>>(
        input_data, num_input_elements, output_data, (CUDA_LONG)num_output_elements, num_per_thread);
  } else {
    int blocksPerGrid = (int)(ceil(static_cast<float>(num_output_elements) / GridDim::maxThreadsPerBlock));
    _TileMemcpyKernel<<<blocksPerGrid, GridDim::maxThreadsPerBlock, 0, stream>>>(
        input_data, num_input_elements, output_data, (CUDA_LONG)num_output_elements);
  }
}

template <typename T>
__global__ void _TileBatchedMemcpyKernel(
    const T* input_data,
    const size_t num_of_elements_per_input_batch,
    const size_t num_input_batch_count,
    const fast_divmod num_of_elements_per_output_batch,
    T* output_data,
    const size_t N) {
  CALCULATE_ELEMENTWISE_INDEX_OR_EXIT(id, N);
  CUDA_LONG batch_idx = 0;
  CUDA_LONG element_idx = 0;
  num_of_elements_per_output_batch.divmod(id, batch_idx, element_idx);
  output_data[id] = input_data[(batch_idx % num_input_batch_count) * num_of_elements_per_input_batch + (element_idx % num_of_elements_per_input_batch)];
}

template <typename T>
void TileBatchedMemcpyImpl(
    cudaStream_t stream,
    const T* input_data,
    const size_t num_of_elements_per_input_batch,
    const size_t num_input_batch_count,
    const fast_divmod& num_of_elements_per_output_batch,
    T* output_data,
    const size_t num_output_elements) {
  int blocksPerGrid = (int)(ceil(static_cast<float>(num_output_elements) / GridDim::maxThreadsPerBlock));
  _TileBatchedMemcpyKernel<<<blocksPerGrid, GridDim::maxThreadsPerBlock, 0, stream>>>(
      input_data,
      num_of_elements_per_input_batch,
      num_input_batch_count,
      num_of_elements_per_output_batch,
      output_data,
      (CUDA_LONG)num_output_elements);
}

#define SPECIALIZED_IMPL(T)                                                                                                                                                                                                                \
  template void TileImpl<T>(cudaStream_t stream, const size_t shape_rank, const TArray<fast_divmod>& fdm_input_shape, const TArray<int64_t>& input_stride, const T* input_data, const TArray<fast_divmod>& fdm_output_strides, T* output_data, const size_t N); \
  template void TileMemcpyImpl<T>(cudaStream_t stream, const T* input_data, const size_t num_input_elements, T* output_data, const size_t num_output_elements);                                                                                                 \
  template void TileBatchedMemcpyImpl<T>(cudaStream_t stream, const T* input_data, const size_t num_of_elements_per_input_batch, const size_t num_input_batch_count, const fast_divmod& num_of_elements_per_output_batch, T* output_data, const size_t num_output_elements);

SPECIALIZED_IMPL(float)
SPECIALIZED_IMPL(double)
SPECIALIZED_IMPL(half)

}  // namespace cuda
}  // namespace onnxruntime
