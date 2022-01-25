# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation.  All rights reserved.
# Licensed under the MIT License.  See License.txt in the project root for
# license information.
# --------------------------------------------------------------------------
"""
JIT interface implementing packed functions that
import and compile frontend models
"""
from .ort import ANSOR_TYPE, AUTO_TVM_TYPE, run_with_benchmark, run_without_benchmark, onnx_compile
