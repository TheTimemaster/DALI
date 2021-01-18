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

#include "dali/operators/image/paste/multipaste.h"
#include "dali/kernels/imgproc/paste/paste.h"
#include "dali/core/tensor_view.h"

namespace dali {
namespace {

template <typename Out, typename In>
using TheKernel = kernels::PasteCpu<Out, In>;

}  // namespace


DALI_SCHEMA(MultiPaste)
.DocStr(R"code(Performs multiple pastes from image batch to each of outputs

This operator can also change the type of data.)code")
.NumInput(1)
.InputDox(0, "images", "3D TensorList", R"code(Batch of input images.

Assumes HWC layout.)code")
.AddArg("in_ids", R"code(1D TensorList of type int.

Indexes from what inputs to paste data in each iteration.)code", DALI_INT32, true)
.AddOptionalArg<int64_t>("in_anchors", R"code(2D TensorList of type int64

Absolute values of LU corner of the selection for each iteration.
Zeros are used if this is omitted.)code", nullptr, true)
.AddOptionalArg<int64_t>("shapes", R"code(2D TensorList of type int64

Absolute values of size of the selection for each iteration.
Input size is used if this is omitted.)code", nullptr, true)
.AddOptionalArg<int64_t>("out_anchors", R"code(2D TensorList of type int64

Absolute values of LU corner of the paste for each iteration.
Zeros are used if omitted.)code", nullptr, true)
.AddArg("output_size",
R"code(Output size.)code", DALI_INT_VEC, true)
.AddOptionalArg("dtype",
R"code(Output data type. If not set, the input type is used.)code", DALI_NO_TYPE)
.NumOutput(1);

DALI_REGISTER_OPERATOR(MultiPaste, MultiPasteCpu, CPU)

bool MultiPasteCpu::SetupImpl(std::vector<OutputDesc> &output_desc,
                                      const workspace_t<CPUBackend> &ws) {
  AcquireArguments(spec_, ws);

  const auto &images = ws.template InputRef<CPUBackend>(0);
  const auto &output = ws.template OutputRef<CPUBackend>(0);
  output_desc.resize(1);

  TYPE_SWITCH(images.type().id(), type2id, InputType, (uint8_t, int16_t, int32_t, float), (
      TYPE_SWITCH(output_type_, type2id, OutputType, (uint8_t, int16_t, int32_t, float), (
          {
            using Kernel = TheKernel<OutputType, InputType>;
            kernel_manager_.Initialize<Kernel>();
            TensorListShape<> sh = images.shape();
            TensorListShape<> shapes(sh.num_samples(), sh.sample_dim());
            for (int i = 0; i < sh.num_samples(); i++) {
              const TensorShape<> &out_sh =
                dali::TensorShape<>(output_size_[i].data[0], output_size_[i].data[1], sh[i][2]);
              shapes.set_tensor_shape(i, out_sh);
            }

            TypeInfo type;
            type.SetType<OutputType>(output_type_);
            output_desc[0] = {shapes, type};
          }
      ), DALI_FAIL(make_string("Unsupported output type: ", output_type_)))  // NOLINT
  ), DALI_FAIL(make_string("Unsupported input type: ", images.type().id())))  // NOLINT
  return true;
}




void MultiPasteCpu::RunImpl(workspace_t<CPUBackend> &ws) {
  const auto &images = ws.template InputRef<CPUBackend>(0);
  auto &output = ws.template OutputRef<CPUBackend>(0);

  output.SetLayout(images.GetLayout());
  auto out_shape = output.shape();

  auto& tp = ws.GetThreadPool();

  auto batch_size = output.shape().num_samples();

  TYPE_SWITCH(images.type().id(), type2id, InputType, (uint8_t, int16_t, int32_t, float), (
      TYPE_SWITCH(output_type_, type2id, OutputType, (uint8_t, int16_t, int32_t, float), (
          {
            using Kernel = TheKernel<OutputType, InputType>;
            for (int i = 0; i < batch_size; i++) {
              auto paste_count = in_idx_[i].shape[0];

              auto to_zero = view<OutputType, 3>(output[i]);
              memset(to_zero.data, 0, to_zero.shape[0] * to_zero.shape[1] * to_zero.shape[2]);

              if (no_intersections_[i]) {
                for (int iter = 0; iter < paste_count; iter++) {
                  int from_sample = in_idx_[i].data[iter];
                  int to_sample = i;

                  tp.AddWork(
                      [&, i, iter, from_sample, to_sample](int thread_id) {
                        kernels::KernelContext ctx;
                        auto tvin = view<const InputType, 3>(images[from_sample]);
                        auto tvout = view<OutputType, 3>(output[to_sample]);

                        auto in_anchor_view = GetInAnchors(i, iter);
                        auto in_shape_view = GetShape(i, iter, Coords(
                                    images.shape()[from_sample].data(), dali::TensorShape<>(2)));
                        auto out_anchor_view = GetOutAnchors(i, iter);

                        kernel_manager_.Run<Kernel>(thread_id, to_sample, ctx, tvout, tvin,
                                        in_anchor_view, in_shape_view, out_anchor_view);
                      },
                      out_shape.tensor_size(to_sample));
                }
              } else {
                tp.AddWork(
                    [&, i, paste_count](int thread_id) {
                      for (int iter = 0; iter < paste_count; iter++) {
                        int from_sample = in_idx_[i].data[iter];
                        int to_sample = i;

                        kernels::KernelContext ctx;
                        auto tvin = view<const InputType, 3>(images[from_sample]);
                        auto tvout = view<OutputType, 3>(output[to_sample]);

                        auto in_anchor_view = GetInAnchors(i, iter);
                        auto in_shape_view = GetShape(i, iter, Coords(
                            images.shape()[from_sample].data(), dali::TensorShape<>(2)));
                        auto out_anchor_view = GetOutAnchors(i, iter);

                        kernel_manager_.Run<Kernel>(thread_id, to_sample, ctx, tvout, tvin,
                                        in_anchor_view, in_shape_view, out_anchor_view);
                      }
                    },
                    paste_count);
              }
            }
          }
      ), DALI_FAIL(make_string("Unsupported output type: ", output_type_)))  // NOLINT
  ), DALI_FAIL(make_string("Unsupported input type: ", images.type().id())))  // NOLINT
  tp.RunAll();
}

}  // namespace dali
