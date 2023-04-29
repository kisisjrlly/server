# Copyright 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import unittest
import tritonclient.grpc as grpcclient
from tritonclient.utils import InferenceServerException


class TestTrtErrorPropagation(unittest.TestCase):

    def setUp(self):
        # Initialize client
        self.__triton = grpcclient.InferenceServerClient("localhost:8001",
                                                         verbose=True)

    def test_invalid_trt_model(self):
        with self.assertRaises(InferenceServerException) as cm:
            self.__triton.load_model("invalid_plan_file")
        got_err_msg = str(cm.exception)
        # "[err_msg]" -> "[triton_err_msg]: [backend_err_msg]: [trt_err_msg]"
        expected_backend_err_msg = "Internal: unable to create TensorRT engine: "
        self.assertIn(
            expected_backend_err_msg, got_err_msg,
            "Cannot find the expected error message from TensorRT backend")
        got_triton_err_msg, got_trt_err_msg = got_err_msg.split(
            expected_backend_err_msg)
        for expected_triton_err_msg_part in [
                "load failed for model", "version 1 is at UNAVAILABLE state: "
        ]:
            self.assertIn(
                expected_triton_err_msg_part, got_triton_err_msg,
                "Cannot find an expected part of error message from Triton")
        for expected_trt_err_msg_part in ["Error Code ", "Internal Error "]:
            self.assertIn(
                expected_trt_err_msg_part, got_trt_err_msg,
                "Cannot find an expected part of error message from TensorRT framework"
            )

    def test_invalid_trt_model_autocomplete(self):
        with self.assertRaises(InferenceServerException) as cm:
            self.__triton.load_model("invalid_plan_file")
        err_msg = str(cm.exception)
        self.assertIn(
            "Internal: unable to load plan file to auto complete config",
            err_msg, "Caught an unexpected exception")


if __name__ == '__main__':
    unittest.main()
