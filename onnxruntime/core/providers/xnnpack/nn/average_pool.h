// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/framework/op_kernel.h"
#include "core/framework/allocator.h"
#include "core/providers/cpu/nn/pool_attributes.h"
#include "core/providers/utils.h"
#include "core/providers/xnnpack/detail/utils.h"
#include "xnnpack.h"

namespace onnxruntime {
class GraphViewer;
class NodeUnit;
namespace xnnpack {

class AveragePool : public OpKernel {
 public:
  explicit AveragePool(const OpKernelInfo& info);

  Status Compute(OpKernelContext* context) const override;
  static bool IsAveragePoolOnnxNodeSupported(const NodeUnit& nodeunit, const GraphViewer& graph);

 private:
  const PoolAttributes pool_attrs_;
  TensorShapeVector output_dims_;

  XnnpackOperator op0_;
  std::optional<std::pair<float, float>> clip_min_max_;
  enum InputTensors : int {
    IN_X = 0,
    IN_X_SCALE = 1,
    IN_X_ZERO_POINT = 2,
    IN_Y_SCALE = 3,
    IN_Y_ZERO_POINT = 4,
  };
  QuantParam quant_param_;
  OpComputeType avgpool_type_ = OpComputeType::op_compute_type_invalid;
};
}  // namespace xnnpack
}  // namespace onnxruntime