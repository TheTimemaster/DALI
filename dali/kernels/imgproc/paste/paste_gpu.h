// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
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

// THIS WHOLE CODE IS A COPY-PASTE OF multiply-add kernel

#ifndef DALI_KERNELS_IMGPROC_PASTE_PASTE_GPU_H_
#define DALI_KERNELS_IMGPROC_PASTE_PASTE_GPU_H_

#include <vector>
#include "dali/core/span.h"
#include "dali/core/convert.h"
#include "dali/core/geom/box.h"
#include "dali/kernels/imgproc/roi.h"
#include "dali/kernels/common/block_setup.h"
#include "dali/kernels/imgproc/paste/paste_gpu_input.h"

namespace dali {
namespace kernels {
namespace paste {

template<class InputType, int ndims>
struct GridCellGPU {
    const InputType *in;
    ivec<ndims> cell_start, cell_end, in_anchor, in_pitch;
};

template <class OutputType, class InputType, int ndims>
struct SampleDescriptorGPU {
  OutputType *out;
  int grid_cell_start_idx;
  ivec<ndims> cell_counts, out_pitch;
};

template <int ndim>
ivec<ndim - 2> pitch_flatten_channels(const TensorShape<ndim> &shape) {
  ivec<ndim - 2> ret;
  int stride = shape[ndim - 1];  // channels
  for (int i = ndim - 2; i > 0; i--) {
    stride *= shape[i];
    ret[ndim - 2 - i] = stride;  // x dimension is at ret[0] - use reverse indexing
  }
  return ret;
}

/**
 * Note: Since the operation we perform is channel-agnostic (it is performed in the same
 * way for every channel), SampleDescriptor assumes, that sample is channel-agnostic. Therefore it
 * needs to be flattened
 * @see FlattenChannels
 */
template <class OutputType, class InputType, int ndims>
void CreateSampleDescriptors(
    span<SampleDescriptorGPU<OutputType, InputType, ndims - 1>> out_descs,
    span<GridCellGPU<InputType, ndims - 1>> out_grid_cells,
    const OutListGPU<OutputType, ndims> &out,
    const InListGPU<InputType, ndims> &in,
    span<paste::MultiPasteSampleInput<ndims - 1>> samples,
    span<paste::GridCellInput<ndims - 1>> grid, int channels) {
  assert(out_descs.size() >= in.num_samples());

  for (int i = 0; i < samples.size(); i++) {
    auto &cpu_sample = samples[i];
    auto &gpu_sample = out_descs[i];
    gpu_sample.out = out[i].data;
    gpu_sample.grid_cell_start_idx = cpu_sample.grid_cell_start_idx;
    gpu_sample.cell_counts = cpu_sample.cell_counts;
    gpu_sample.out_pitch[1] = out[i].shape[1];

    gpu_sample.out_pitch[1] *= channels;
  }

  for (int i = 0; i < grid.size(); i++) {
    auto &cpu_grid_cell = grid[i];
    auto &gpu_grid_cell = out_grid_cells[i];

    gpu_grid_cell.in = cpu_grid_cell.input_idx == -1 ? nullptr : in[cpu_grid_cell.input_idx].data;
    gpu_grid_cell.cell_start = cpu_grid_cell.cell_start;
    gpu_grid_cell.cell_end = cpu_grid_cell.cell_end;
    gpu_grid_cell.in_anchor = cpu_grid_cell.in_anchor;
    gpu_grid_cell.in_pitch[1] =
            cpu_grid_cell.input_idx == -1 ? -1 : in[cpu_grid_cell.input_idx].shape[1];

    gpu_grid_cell.cell_start[1] *= channels;
    gpu_grid_cell.cell_end[1] *= channels;
    gpu_grid_cell.in_anchor[1] *= channels;
    gpu_grid_cell.in_pitch *= channels;
  }
}


template <class OutputType, class InputType, int ndims>
__global__ void
PasteKernel(const SampleDescriptorGPU<OutputType, InputType, ndims> *samples,
                  const GridCellGPU<InputType, ndims> *grid_cells,
                  const BlockDesc<ndims> *blocks) {
  static_assert(ndims == 2, "Function requires 2 dimensions in the input");
  const auto &block = blocks[blockIdx.x];
  const auto &sample = samples[block.sample_idx];
  const GridCellGPU<InputType, ndims> *my_grid_cells = grid_cells + sample.grid_cell_start_idx;


  auto *__restrict__ out = sample.out;

  int grid_y = 0;
  int min_grid_x = 0;
  while (threadIdx.x + block.start.x >= my_grid_cells[min_grid_x].cell_end[1]) min_grid_x++;

  for (int y = threadIdx.y + block.start.y; y < block.end.y; y += blockDim.y) {
    while (y >= my_grid_cells[grid_y * sample.cell_counts[1]].cell_end[0]) grid_y++;
    int grid_x = min_grid_x;
    for (int x = threadIdx.x + block.start.x; x < block.end.x; x += blockDim.x) {
      while (x >= my_grid_cells[grid_y * sample.cell_counts[1] + grid_x].cell_end[1]) grid_x++;
      const GridCellGPU<InputType, ndims> *cell =
              &my_grid_cells[grid_y * sample.cell_counts[1] + grid_x];
      if (cell->in == nullptr) {
          out[y * sample.out_pitch[1] + x] = 0;
      } else {
        out[y * sample.out_pitch[1] + x] = ConvertSat<OutputType>(
                cell->in[(y - cell->cell_start[0] + cell->in_anchor[0]) * cell->in_pitch[1]
                         + (x - cell->cell_start[1] + cell->in_anchor[1])]);
      }
    }
  }
}

}  // namespace paste

template <typename OutputType, typename InputType, int ndims>
class PasteGPU {
 private:
  static constexpr size_t spatial_dims = ndims - 1;
  using BlockDesc = kernels::BlockDesc<spatial_dims>;
  using SampleDesc = paste::SampleDescriptorGPU<OutputType, InputType, spatial_dims>;
  using GridCellDesc = paste::GridCellGPU<InputType, spatial_dims>;

  std::vector<SampleDesc> sample_descriptors_;
  std::vector<GridCellDesc> grid_cell_descriptors_;

 public:
  BlockSetup<spatial_dims, -1 /* No channel dimension, only spatial */> block_setup_;

  KernelRequirements Setup(
      KernelContext &context,
      const InListGPU<InputType, ndims> &in,
      span<paste::MultiPasteSampleInput<spatial_dims>> samples,
      span<paste::GridCellInput<spatial_dims>> grid_cells,
      TensorListShape<ndims> out_shape) {
    DALI_ENFORCE([=]() -> bool {
      auto ref_nchannels = in.shape[0][ndims - 1];
      for (int i = 0; i < in.num_samples(); i++) {
        if (in.shape[i][ndims - 1] != ref_nchannels) {
          return false;
        }
      }
      return true;
    }(), "Number of channels for every image in batch must be equal");

    KernelRequirements req;
    ScratchpadEstimator se;
    auto flattened_shape = collapse_dim(out_shape, 1);
    block_setup_.SetupBlocks(flattened_shape, true);
    sample_descriptors_.resize(samples.size());
    grid_cell_descriptors_.resize(grid_cells.size());
    se.add<SampleDesc>(AllocType::GPU, samples.size());
    se.add<GridCellDesc>(AllocType::GPU, grid_cells.size());
    se.add<BlockDesc>(AllocType::GPU, block_setup_.Blocks().size());
    // req.output_shapes = {in.shape}; Once again, this is determined by operator
    req.scratch_sizes = se.sizes;
    return req;
  }


  void Run(
      KernelContext &context,
      const OutListGPU<OutputType, ndims> &out, const InListGPU<InputType, ndims> &in,
      span<paste::MultiPasteSampleInput<spatial_dims>> samples,
      span<paste::GridCellInput<spatial_dims>> grid) {
    paste::CreateSampleDescriptors(
        make_span(sample_descriptors_),
        make_span(grid_cell_descriptors_), out, in, samples, grid, 3);

    SampleDesc *samples_gpu;
    GridCellDesc  *grid_cells_gpu;
    BlockDesc *blocks_gpu;

    std::tie(samples_gpu, grid_cells_gpu, blocks_gpu) = context.scratchpad->ToContiguousGPU(
        context.gpu.stream, sample_descriptors_, grid_cell_descriptors_, block_setup_.Blocks());

    dim3 grid_dim = block_setup_.GridDim();
    dim3 block_dim = block_setup_.BlockDim();
    auto stream = context.gpu.stream;

    paste::PasteKernel<<<grid_dim, block_dim, 0, stream>>>(samples_gpu, grid_cells_gpu, blocks_gpu);
  }
};

}  // namespace kernels
}  // namespace dali
#endif  // DALI_KERNELS_IMGPROC_PASTE_PASTE_GPU_H_
