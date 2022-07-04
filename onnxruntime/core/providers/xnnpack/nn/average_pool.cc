// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <string>

#include "core/providers/xnnpack/nn/average_pool.h"

#include "core/common/status.h"
#include "core/providers/xnnpack/detail/utils.h"
#include "core/graph/graph.h"
#include "core/providers/utils.h"
#include "core/framework/tensorprotoutils.h"

#include <xnnpack.h>

namespace onnxruntime {
namespace xnnpack {
namespace {
Status CreateXnnpackKernel(const PoolAttributes& pool_attrs,
                           int64_t C,
                           const std::optional<std::pair<float, float>>& clip_min_max,
                           struct xnn_operator*& p,
                           QuantParam* quant_param,
                           OpComputeType avgpool_type) {
  uint32_t input_padding_top = gsl::narrow<uint32_t>(pool_attrs.pads[0]);
  uint32_t input_padding_left = gsl::narrow<uint32_t>(pool_attrs.pads[1]);
  uint32_t input_padding_bottom = gsl::narrow<uint32_t>(pool_attrs.pads[2]);
  uint32_t input_padding_right = gsl::narrow<uint32_t>(pool_attrs.pads[3]);

  uint32_t pooling_height = gsl::narrow<uint32_t>(pool_attrs.kernel_shape[0]);
  uint32_t pooling_width = gsl::narrow<uint32_t>(pool_attrs.kernel_shape[1]);
  uint32_t stride_height = gsl::narrow<uint32_t>(pool_attrs.strides[0]);
  uint32_t stride_width = gsl::narrow<uint32_t>(pool_attrs.strides[1]);

  uint32_t flags = 0;
  if (pool_attrs.auto_pad == AutoPadType::SAME_UPPER) {
    flags |= XNN_FLAG_TENSORFLOW_SAME_PADDING;
  }
  xnn_status status;
  if (avgpool_type == OpComputeType::op_compute_type_fp32) {
    float output_min = clip_min_max ? clip_min_max->first : -INFINITY;
    float output_max = clip_min_max ? clip_min_max->second : INFINITY;

    status = xnn_create_average_pooling2d_nhwc_f32(input_padding_top, input_padding_right,
                                                   input_padding_bottom, input_padding_left,
                                                   pooling_height, pooling_width,
                                                   stride_height, stride_width,
                                                   C, C, C,  // channels, input_pixel_stride, output_pixel_stride
                                                   output_min, output_max, flags, &p);
  } else if (avgpool_type == OpComputeType::op_compute_type_qu8) {
    uint8_t output_min = clip_min_max ? gsl::narrow<uint8_t>(clip_min_max->first) : 0;
    uint8_t output_max = clip_min_max ? gsl::narrow<uint8_t>(clip_min_max->second) : 255;
    status = xnn_create_average_pooling2d_nhwc_qu8(input_padding_top, input_padding_right,
                                                   input_padding_bottom, input_padding_left,
                                                   pooling_height, pooling_width,
                                                   stride_height, stride_width,
                                                   C, C, C,  // channels, input_pixel_stride, output_pixel_stride
                                                   quant_param->X_zero_point_value,
                                                   quant_param->X_scale_value,
                                                   quant_param->Y_zero_point_value,
                                                   quant_param->Y_scale_value,
                                                   output_min, output_max, flags, &p);
  } else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "error kernel type input, expected uint8|float");
  }
  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "xnn_create_average_pooling2d_nhwc_ failed. Status:", status);
  }
  return Status::OK();
}

static bool IsQuantAvgPoolSupported(const NodeUnit& node_unit, const GraphViewer& graph) {
  bool supported = false;
  do {
    xnn_datatype x_input_type, output_type;
    const auto& inputs = node_unit.Inputs();
    if (inputs.size() != 1) {
      break;
    }
    x_input_type = GetDtypeInXnnpack(node_unit, 0, false, graph);
    output_type = GetDtypeInXnnpack(node_unit, 0, true, graph);
    if (x_input_type != xnn_datatype_quint8 ||
        output_type != xnn_datatype_quint8) {
      break;
    }
    supported = true;
  } while (false);

  return supported;
}
}  // namespace

bool AveragePool::IsAveragePoolOnnxNodeSupported(const onnxruntime::NodeUnit& nodeunit,
                                                 const onnxruntime::GraphViewer& graph) {
  bool supported = false;

  if (IsQuantizedAvgPool(GetQuantizedOpType(nodeunit)) && IsQuantAvgPoolSupported(nodeunit, graph) == false) {
    return supported;
  }
  const onnxruntime::Node& node = nodeunit.GetNode();
  const auto& inputs = nodeunit.Inputs();
  // use do {} while(false) so it's easier to set a breakpoint on the return
  do {
    // AveragePool has 1 input.
    const auto& x_arg = inputs[0].node_arg;

    // we only support 2D (4 dims with batch and channel)
    const auto* x_shape = x_arg.Shape();
    if (!x_shape || x_shape->dim_size() != 4) {
      break;
    }

    // require C, H, W to be known so we can construct the xnnpack kernel prior to Compute
    if (!x_shape->dim(1).has_dim_value() ||
        !x_shape->dim(2).has_dim_value() ||
        !x_shape->dim(3).has_dim_value()) {
      break;
    }

    onnxruntime::ProtoHelperNodeContext nc(node);
    onnxruntime::OpNodeProtoHelper info(&nc);
    onnxruntime::PoolAttributes pool_attrs(info, "AveragePool", node.SinceVersion());

    // xnnpack doesn't appear to support using 'ceil' to calculate the output shape
    // https://github.com/google/XNNPACK/blob/3caa8b9de973839afa1e2a1462ff356e6927a66b/src/operators/average-pooling-nhwc.c#L643
    // calls compute_output_dimension but there's no ability to specify rounding that value up.
    if (pool_attrs.ceil_mode != 0) {
      break;
    }

    if (!IsPaddingTypeSupported(pool_attrs.auto_pad)) {
      break;
    }

    if ((pool_attrs.kernel_shape.size() != 2) ||
        (pool_attrs.kernel_shape[0] == 1 && pool_attrs.kernel_shape[1] == 1)) {
      // XNNPack doesn't support 1x1 average pool.
      break;
    }
    // Average-pool has no multi-outputs definition in ONNX
    supported = true;
  } while (false);

  return supported;
}

AveragePool::AveragePool(const OpKernelInfo& info)
    : OpKernel(info),
      pool_attrs_{info, "AveragePool", info.node().SinceVersion()} {
  // get values from any fusion with an activation
  if (std::string activation; info.GetAttr<std::string>("activation", &activation).IsOK()) {
    if (activation == "Clip" || activation == "Relu") {
      std::vector<float> activation_params;

      // min/max could be from Clip or Relu
      if (info.GetAttrs<float>("activation_params", activation_params).IsOK()) {
        if (activation_params.size() == 2) {
          clip_min_max_ = {activation_params[0], activation_params[1]};
        }
      }
    }
  }

  // input is NHWC and we only support input with 4 dims. we checked C, H, W were all known in the op support checker
  const auto& X_arg = *Node().InputDefs()[0];
  const auto& X_shape = *X_arg.Shape();
  int64_t H = X_shape.dim(1).dim_value();
  int64_t W = X_shape.dim(2).dim_value();
  int64_t C = X_shape.dim(3).dim_value();

  // create NCHW shape to calculate most of the output shape. 'N' is set in Compute.
  TensorShapeVector input_shape{1, C, H, W};
  auto pads = pool_attrs_.pads;
  auto nchw_output_dims = pool_attrs_.SetOutputSize(input_shape, C, &pads);
  output_dims_ = {-1, nchw_output_dims[2], nchw_output_dims[3], nchw_output_dims[1]};

  // TEMPORARY sanity check. If C, H and W are known, the output shape should have been able to be inferred, with the
  // exception of the batch size. Can be removed once we've run more models using xnnpack AveragePool.
  auto inferred_output_shape = utils::GetTensorShapeFromTensorShapeProto(*Node().OutputDefs()[0]->Shape());
  ORT_ENFORCE(inferred_output_shape[1] == output_dims_[1] &&
                  inferred_output_shape[2] == output_dims_[2] &&
                  inferred_output_shape[3] == output_dims_[3],
              "Shape mismatch between inferred value and calculated value.");
  const auto& input_dtype = X_arg.TypeAsProto()->tensor_type().elem_type();
  if (input_dtype == ONNX_NAMESPACE::TensorProto_DataType_FLOAT) {
    avgpool_type_ = OpComputeType::op_compute_type_fp32;
  } else if (input_dtype == ONNX_NAMESPACE::TensorProto_DataType_UINT8) {
    InputTensorOrder tensor_index = {0, 1, 2, -1, -1, -1, 3, 4, -1};
    ParseQuantParamFromInfoByOrder(info, tensor_index, quant_param_);
    /*
    const Tensor* X_zero_point = nullptr;
    const Tensor* Y_zero_point = nullptr;
    const Tensor* X_scale = nullptr;
    const Tensor* Y_scale = nullptr;
    // we have check it in op_checker already
    info.TryGetConstantInput(InputTensors::IN_X_SCALE, &X_scale);
    info.TryGetConstantInput(InputTensors::IN_X_ZERO_POINT, &X_zero_point);
    info.TryGetConstantInput(InputTensors::IN_Y_SCALE, &Y_scale);
    info.TryGetConstantInput(InputTensors::IN_Y_ZERO_POINT, &Y_zero_point);

    // IsScalarOr1ElementVector(X_scale),
    // X_zero_point == nullptr || IsScalarOr1ElementVector(X_zero_point),
    // IsScalarOr1ElementVector(Y_scale),
    // Y_zero_point == nullptr || IsScalarOr1ElementVector(Y_zero_point),

    quant_param_.X_zero_point_value = *(X_zero_point->template Data<uint8_t>());
    quant_param_.X_scale_value = *(X_scale->template Data<float>());
    quant_param_.Y_zero_point_value = *(Y_zero_point->template Data<uint8_t>());
    quant_param_.Y_scale_value = *(Y_scale->template Data<float>());
    */
    avgpool_type_ = OpComputeType::op_compute_type_qu8;
  }
  struct xnn_operator* p;
  auto ret = CreateXnnpackKernel(pool_attrs_, C, clip_min_max_, p,
                                 &quant_param_, avgpool_type_);
  ORT_ENFORCE(ret.IsOK(), ret.ErrorMessage());
  op0_.reset(p);
}

Status AveragePool::Compute(OpKernelContext* context) const {
  const auto& X = *context->Input<Tensor>(0);
  const auto& X_shape = X.Shape();

  int64_t N = X_shape[0];
  int64_t H = X_shape[1];
  int64_t W = X_shape[2];

  // set the N dim to the correct value
  TensorShapeVector output_dims{output_dims_};
  output_dims[0] = N;
  Tensor* Y = context->Output(0, output_dims);

  // empty input
  if (Y->Shape().Size() == 0) {
    return Status::OK();
  }

  xnn_status status = xnn_status_invalid_state;
  if (avgpool_type_ == OpComputeType::op_compute_type_fp32) {
    status = xnn_setup_average_pooling2d_nhwc_f32(op0_.get(), N, H, W,
                                                  X.Data<float>(), Y->MutableData<float>(),
                                                  nullptr /*threadpool */);  // TBD: how to handle threading
  } else if (avgpool_type_ == OpComputeType::op_compute_type_qu8) {
    status = xnn_setup_average_pooling2d_nhwc_qu8(op0_.get(), N, H, W,
                                                  X.Data<uint8_t>(), Y->MutableData<uint8_t>(),
                                                  nullptr /*threadpool */);  // TBD: how to handle threading
  }

  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "xnn_setup_average_pooling2d_nhwc_ returned ", status);
  }

  status = xnn_run_operator(op0_.get(), nullptr);
  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "xnn_run_operator returned ", status);
  }

  return Status::OK();
}

ONNX_OPERATOR_VERSIONED_KERNEL_EX(AveragePool, kMSInternalNHWCDomain, 7, 7, kXnnpackExecutionProvider,
                                  KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
                                  AveragePool);
ONNX_OPERATOR_TYPED_KERNEL_EX(
    QLinearAveragePool,
    kMSDomain,
    1,
    uint8_t,
    kXnnpackExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T", DataTypeImpl::GetTensorType<uint8_t>()),
    AveragePool);

}  // namespace xnnpack
}  // namespace onnxruntime