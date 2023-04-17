// Copyright 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

constexpr const char* GLOBAL_OPTION_GROUP = "";

#ifdef TRITON_ENABLE_LOGGING
constexpr bool ENABLE_LOGGING = true;
#else
constexpr bool ENABLE_LOGGING = false;
#endif  // TRITON_ENABLE_LOGGING

#ifdef TRITON_ENABLE_HTTP
constexpr bool ENABLE_HTTP = true;
#else
constexpr bool ENABLE_HTTP = false;
#endif  // TRITON_ENABLE_HTTP

#ifdef TRITON_ENABLE_GRPC
constexpr bool ENABLE_GRPC = true;
#else
constexpr bool ENABLE_GRPC = false;
#endif  // TRITON_ENABLE_GRPC

#ifdef TRITON_ENABLE_METRICS
constexpr bool ENABLE_METRICS = true;
#else
constexpr bool ENABLE_METRICS = false;
#endif  // TRITON_ENABLE_METRICS

#ifdef TRITON_ENABLE_TRACING
constexpr bool ENABLE_TRACING = true;
#else
constexpr bool ENABLE_TRACING = false;
#endif  // TRITON_ENABLE_TRACING

#ifdef TRITON_ENABLE_SAGEMAKER
constexpr bool ENABLE_SAGEMAKER = true;
#else
constexpr bool ENABLE_SAGEMAKER = false;
#endif  // TRITON_ENABLE_SAGEMAKER

#ifdef TRITON_ENABLE_VERTEX_AI
constexpr bool ENABLE_VERTEX_AI = true;
#else
constexpr bool ENABLE_VERTEX_AI = false;
#endif  // TRITON_ENABLE_VERTEX_AI
