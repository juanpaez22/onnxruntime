// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/optimizer/graph_transformer.h"

namespace onnxruntime {

/**
@Class FusedSoftmaxFusion

Fuse Where + Softmax to FusedSoftmaxFusion

*/
class FusedSoftmaxFusion : public GraphTransformer {
 public:
  FusedSoftmaxFusion(const std::unordered_set<std::string>& compatible_execution_providers = {}) noexcept
      : GraphTransformer("FusedSoftmaxFusion", compatible_execution_providers) {}

  Status ApplyImpl(Graph& graph, bool& modified, int graph_level, const logging::Logger& logger) const override;
};

}  // namespace onnxruntime