// Copyright 2019-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "tracer.h"

#include <stdlib.h>
#include <unordered_map>
#include "common.h"
#include "triton/common/logging.h"
#ifdef TRITON_ENABLE_GPU
#include <cuda_runtime_api.h>
#endif  // TRITON_ENABLE_GPU
#ifndef _WIN32
namespace otlp = opentelemetry::exporter::otlp;
namespace otel_trace_sdk = opentelemetry::sdk::trace;
namespace otel_trace_api = opentelemetry::trace;
namespace otel_common = opentelemetry::common;
#endif

namespace triton { namespace server {

TRITONSERVER_Error*
TraceManager::Create(
    TraceManager** manager, const TRITONSERVER_InferenceTraceLevel level,
    const uint32_t rate, const int32_t count, const uint32_t log_frequency,
    const std::string& filepath, const InferenceTraceMode mode,
    const triton::server::TraceConfigMap& config_map)
{
  // Always create TraceManager regardless of the global setting as they
  // can be updated at runtime even if tracing is not enable at start.
  // No trace should be sampled if the setting is not valid.
  *manager = new TraceManager(
      level, rate, count, log_frequency, filepath, mode, config_map);

  return nullptr;  // success
}

TraceManager::TraceManager(
    const TRITONSERVER_InferenceTraceLevel level, const uint32_t rate,
    const int32_t count, const uint32_t log_frequency,
    const std::string& filepath, const InferenceTraceMode mode,
    const TraceConfigMap& config_map)
{
  std::shared_ptr<TraceFile> file(new TraceFile(filepath));
  global_default_.reset(new TraceSetting(
      level, rate, count, log_frequency, file, mode, config_map,
      false /*level_specified*/, false /*rate_specified*/,
      false /*count_specified*/, false /*log_frequency_specified*/,
      false /*filepath_specified*/, false /*mode_specified*/,
      false /*config_map_specified*/));
  global_setting_.reset(new TraceSetting(
      level, rate, count, log_frequency, file, mode, config_map,
      false /*level_specified*/, false /*rate_specified*/,
      false /*count_specified*/, false /*log_frequency_specified*/,
      false /*filepath_specified*/, false /*mode_specified*/,
      false /*config_map_specified*/));
  trace_files_.emplace(filepath, file);
}

TRITONSERVER_Error*
TraceManager::UpdateTraceSetting(
    const std::string& model_name, const NewSetting& new_setting)
{
  std::lock_guard<std::mutex> w_lk(w_mu_);

  RETURN_IF_ERR(UpdateTraceSettingInternal(model_name, new_setting));
  // If updating global setting, must check and update the model settings
  // that are (partially) mirroring global setting.
  if (model_name.empty()) {
    // Default constructed setting means no active update,
    // only the unspecified fields will be checked and updated.
    NewSetting setting;
    // Make a copy of the set as UpdateTraceSettingInternal() may modify
    // 'fallback_used_models_'
    auto fallback_models = fallback_used_models_;
    for (const auto& name : fallback_models) {
      RETURN_IF_ERR(UpdateTraceSettingInternal(name, setting));
    }
  }
  return nullptr;
}

TRITONSERVER_Error*
TraceManager::UpdateTraceSettingInternal(
    const std::string& model_name, const NewSetting& new_setting)
{
  // First try to get the current setting and fallback setting,
  // current setting may be 'nullptr' if the setting is newly added
  const TraceSetting* current_setting = nullptr;
  const TraceSetting* fallback_setting = nullptr;
  if (!model_name.empty()) {
    auto it = model_settings_.find(model_name);
    if (it != model_settings_.end()) {
      current_setting = it->second.get();
    }
    fallback_setting = global_setting_.get();
  } else {
    current_setting = global_setting_.get();
    fallback_setting = global_default_.get();
  }

  // Prepare the updated setting, use two passes for simplicity:
  // 1. Set all fields based on 'fallback_setting'
  // 2. If there are specified fields based on current and new setting,
  //    use the specified value
  TRITONSERVER_InferenceTraceLevel level = fallback_setting->level_;
  uint32_t rate = fallback_setting->rate_;
  int32_t count = fallback_setting->count_;
  uint32_t log_frequency = fallback_setting->log_frequency_;
  std::string filepath = fallback_setting->file_->FileName();
  InferenceTraceMode mode = fallback_setting->mode_;
  TraceConfigMap config_map = fallback_setting->config_map_;

  // Whether the field value is specified:
  // if clear then it is not specified, otherwise,
  // it is specified if it is being updated, or it was previously specified
  const bool level_specified =
      (new_setting.clear_level_ ? false
                                : (((current_setting != nullptr) &&
                                    current_setting->level_specified_) ||
                                   (new_setting.level_ != nullptr)));
  const bool rate_specified =
      (new_setting.clear_rate_ ? false
                               : (((current_setting != nullptr) &&
                                   current_setting->rate_specified_) ||
                                  (new_setting.rate_ != nullptr)));
  const bool count_specified =
      (new_setting.clear_count_ ? false
                                : (((current_setting != nullptr) &&
                                    current_setting->count_specified_) ||
                                   (new_setting.count_ != nullptr)));
  const bool log_frequency_specified =
      (new_setting.clear_log_frequency_
           ? false
           : (((current_setting != nullptr) &&
               current_setting->log_frequency_specified_) ||
              (new_setting.log_frequency_ != nullptr)));
  const bool filepath_specified =
      (new_setting.clear_filepath_ ? false
                                   : (((current_setting != nullptr) &&
                                       current_setting->filepath_specified_) ||
                                      (new_setting.filepath_ != nullptr)));
  const bool mode_specified =
      (new_setting.clear_mode_ ? false
                               : (((current_setting != nullptr) &&
                                   current_setting->mode_specified_) ||
                                  (new_setting.mode_ != nullptr)));
  const bool config_map_specified =
      (new_setting.config_map_ ? false
                               : (((current_setting != nullptr) &&
                                   current_setting->config_map_specified_) ||
                                  (new_setting.config_map_ != nullptr)));
  if (level_specified) {
    level = (new_setting.level_ != nullptr) ? *new_setting.level_
                                            : current_setting->level_;
  }
  if (rate_specified) {
    rate = (new_setting.rate_ != nullptr) ? *new_setting.rate_
                                          : current_setting->rate_;
  }
  if (count_specified) {
    count = (new_setting.count_ != nullptr) ? *new_setting.count_
                                            : current_setting->count_;
  }
  if (log_frequency_specified) {
    log_frequency = (new_setting.log_frequency_ != nullptr)
                        ? *new_setting.log_frequency_
                        : current_setting->log_frequency_;
  }
  if (filepath_specified) {
    filepath = (new_setting.filepath_ != nullptr)
                   ? *new_setting.filepath_
                   : current_setting->file_->FileName();
  }
  if (mode_specified) {
    mode = (new_setting.mode_ != nullptr) ? *new_setting.mode_
                                          : current_setting->mode_;
  }
  if (config_map_specified) {
    config_map = (new_setting.config_map_ != nullptr)
                     ? *new_setting.config_map_
                     : current_setting->config_map_;
  }

  // Some special case when updating model setting
  if (!model_name.empty()) {
    bool all_specified =
        (level_specified & rate_specified & count_specified &
         log_frequency_specified & filepath_specified);
    bool none_specified =
        !(level_specified | rate_specified | count_specified |
          log_frequency_specified | filepath_specified);
    if (all_specified) {
      fallback_used_models_.erase(model_name);
    } else if (none_specified) {
      // Simply let the model uses global setting
      std::lock_guard<std::mutex> r_lk(r_mu_);
      model_settings_.erase(model_name);
      return nullptr;
    } else {
      fallback_used_models_.emplace(model_name);
    }
  }

  // Create TraceSetting object with the updated setting
  std::shared_ptr<TraceFile> file;
  const auto it = trace_files_.find(filepath);
  if (it != trace_files_.end()) {
    file = it->second.lock();
    // The TraceFile object is no longer valid
    if (file == nullptr) {
      trace_files_.erase(it);
    }
  }
  if (file == nullptr) {
    file.reset(new TraceFile(filepath));
    trace_files_.emplace(filepath, file);
  }

  std::shared_ptr<TraceSetting> lts(new TraceSetting(
      level, rate, count, log_frequency, file, mode, config_map,
      level_specified, rate_specified, count_specified, log_frequency_specified,
      filepath_specified, mode_specified, config_map_specified));
  // The only invalid setting allowed is if it disables tracing
  if ((!lts->Valid()) && (level != TRITONSERVER_TRACE_LEVEL_DISABLED)) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        (std::string("Attempting to set invalid trace setting :") +
         lts->Reason())
            .c_str());
  }

  // Update / Init the setting in read lock to exclude reader access,
  // we replace the object instead of modifying the existing object in case
  // of there are ongoing traces. This makes sure those traces are referring
  // to the setting when the traces are sampled.
  {
    std::lock_guard<std::mutex> r_lk(r_mu_);
    if (model_name.empty()) {
      // global update
      global_setting_ = std::move(lts);
    } else {
      auto it = model_settings_.find(model_name);
      if (it != model_settings_.end()) {
        // Model update
        it->second = std::move(lts);
      } else {
        // Model init
        model_settings_.emplace(model_name, lts);
      }
    }
  }

  return nullptr;
}

void
TraceManager::GetTraceSetting(
    const std::string& model_name, TRITONSERVER_InferenceTraceLevel* level,
    uint32_t* rate, int32_t* count, uint32_t* log_frequency,
    std::string* filepath, InferenceTraceMode* mode)
{
  std::shared_ptr<TraceSetting> trace_setting;
  {
    std::lock_guard<std::mutex> r_lk(r_mu_);
    auto m_it = model_settings_.find(model_name);
    trace_setting =
        (m_it == model_settings_.end()) ? global_setting_ : m_it->second;
  }

  *level = trace_setting->level_;
  *rate = trace_setting->rate_;
  *count = trace_setting->count_;
  *log_frequency = trace_setting->log_frequency_;
  *filepath = trace_setting->file_->FileName();
  *mode = trace_setting->mode_;
}

std::shared_ptr<TraceManager::Trace>
TraceManager::SampleTrace(const std::string& model_name)
{
  std::shared_ptr<TraceSetting> trace_setting;
  {
    std::lock_guard<std::mutex> r_lk(r_mu_);
    auto m_it = model_settings_.find(model_name);
    trace_setting =
        (m_it == model_settings_.end()) ? global_setting_ : m_it->second;
  }
  std::shared_ptr<Trace> ts = trace_setting->SampleTrace();
  if (ts != nullptr) {
    ts->setting_ = trace_setting;
  }
  return ts;
}

TraceManager::Trace::~Trace()
{
  if (setting_->mode_ == TRACE_MODE_TRITON) {
    // Write trace now
    setting_->WriteTrace(streams_);
  } else if (setting_->mode_ == TRACE_MODE_OPENTELEMETRY) {
#ifndef _WIN32
    this->EndSpan();
#else
    LOG_ERROR << "Unsupported trace mode: "
             << TraceManager::InferenceTraceModeString(setting_->mode_);
#endif
  }
}

void
TraceManager::Trace::CaptureTimestamp(
    const std::string& name, uint64_t timestamp_ns)
{
  if (setting_->level_ & TRITONSERVER_TRACE_LEVEL_TIMESTAMPS) {
    if (setting_->mode_ == TRACE_MODE_TRITON) {
      std::lock_guard<std::mutex> lk(mtx_);
      std::stringstream* ss = nullptr;
      {
        if (streams_.find(trace_id_) == streams_.end()) {
          std::unique_ptr<std::stringstream> stream(new std::stringstream());
          ss = stream.get();
          streams_.emplace(trace_id_, std::move(stream));
        } else {
          ss = streams_[trace_id_].get();
          // If the string stream is not newly created, add "," as there is
          // already content in the string stream
          *ss << ",";
        }
      }
      *ss << "{\"id\":" << trace_id_ << ",\"timestamps\":["
          << "{\"name\":\"" << name << "\",\"ns\":" << timestamp_ns << "}]}";
    } else if (setting_->mode_ == TRACE_MODE_OPENTELEMETRY) {
#ifndef _WIN32
      otel_common::SystemTimestamp otel_timestamp{
          (time_offset_ + std::chrono::nanoseconds{timestamp_ns})};
      if (trace_span_ == nullptr) {
        this->InitSpan(otel_timestamp);
      }
      trace_span_->AddEvent(
          name, otel_timestamp, {{"triton.steady_timestamp_ns", timestamp_ns}});
#else
      LOG_ERROR << "Unsupported trace mode: "
               << TraceManager::InferenceTraceModeString(setting_->mode_);
#endif
    }
  }
}

#ifndef _WIN32
void
TraceManager::Trace::InitTracer(
    const triton::server::TraceConfigMap& config_map)
{
  otlp::OtlpHttpExporterOptions opts;
  auto mode_key = std::to_string(TRACE_MODE_OPENTELEMETRY);
  auto otel_options_it = config_map.find(mode_key);
  if (otel_options_it != config_map.end()) {
    for (const auto& setting : otel_options_it->second) {
      // FIXME add more configuration options of OTLP HTTP Exporter
      if (setting.first == "url") {
        opts.url = setting.second;
      }
    }
  }
  this->exporter_ = otlp::OtlpHttpExporterFactory::Create(opts);
  this->processor_ = otel_trace_sdk::SimpleSpanProcessorFactory::Create(
      std::move(this->exporter_));
  this->provider_ = otel_trace_sdk::TracerProviderFactory::Create(
      std::move(this->processor_));
  otel_trace_api::Provider::SetTracerProvider(this->provider_);
}

void
TraceManager::Trace::InitSpan(otel_common::SystemTimestamp timestamp_ns)
{
  otel_trace_api::StartSpanOptions options;
  options.kind = otel_trace_api::SpanKind::kServer;  // server
  options.start_system_time = timestamp_ns;
  // [FIXME] think about names
  trace_span_ =
      provider_->GetTracer("triton-server")->StartSpan("InferRequest", options);
}

void
TraceManager::Trace::EndSpan()
{
  if (trace_span_ != nullptr) {
    trace_span_->End();
  }
}
#endif

void
TraceManager::TraceRelease(TRITONSERVER_InferenceTrace* trace, void* userp)
{
  uint64_t parent_id;
  LOG_TRITONSERVER_ERROR(
      TRITONSERVER_InferenceTraceParentId(trace, &parent_id),
      "getting trace parent id");
  // The userp will be shared with the trace children, so only delete it
  // if the root trace is being released
  if (parent_id == 0) {
    delete reinterpret_cast<std::shared_ptr<TraceManager::Trace>*>(userp);
  }
  LOG_TRITONSERVER_ERROR(
      TRITONSERVER_InferenceTraceDelete(trace), "deleting trace");
}

const char*
TraceManager::InferenceTraceModeString(InferenceTraceMode mode)
{
  switch (mode) {
    case TRACE_MODE_TRITON:
      return "TRITON";
    case TRACE_MODE_OPENTELEMETRY:
      return "OPENTELEMETRY";
  }

  return "<unknown>";
}

void
TraceManager::TraceActivity(
    TRITONSERVER_InferenceTrace* trace,
    TRITONSERVER_InferenceTraceActivity activity, uint64_t timestamp_ns,
    void* userp)
{
  uint64_t id;
  LOG_TRITONSERVER_ERROR(
      TRITONSERVER_InferenceTraceId(trace, &id), "getting trace id");

  // The function may be called with different traces but the same 'userp',
  // group the activity of the same trace together for more readable output.
  auto ts =
      reinterpret_cast<std::shared_ptr<TraceManager::Trace>*>(userp)->get();


  std::lock_guard<std::mutex> lk(ts->mtx_);
  std::stringstream* ss = nullptr;
  {
    if (ts->setting_->mode_ == TRACE_MODE_TRITON) {
      if (ts->streams_.find(id) == ts->streams_.end()) {
        std::unique_ptr<std::stringstream> stream(new std::stringstream());
        ss = stream.get();
        ts->streams_.emplace(id, std::move(stream));
      } else {
        ss = ts->streams_[id].get();
        // If the string stream is not newly created, add "," as there is
        // already content in the string stream
        *ss << ",";
      }
    }
  }

  // If 'activity' is TRITONSERVER_TRACE_REQUEST_START then collect
  // and serialize trace details.
  if (activity == TRITONSERVER_TRACE_REQUEST_START) {
    const char* model_name;
    int64_t model_version;
    uint64_t parent_id;
    const char* request_id;

    LOG_TRITONSERVER_ERROR(
        TRITONSERVER_InferenceTraceModelName(trace, &model_name),
        "getting model name");
    LOG_TRITONSERVER_ERROR(
        TRITONSERVER_InferenceTraceModelVersion(trace, &model_version),
        "getting model version");
    LOG_TRITONSERVER_ERROR(
        TRITONSERVER_InferenceTraceParentId(trace, &parent_id),
        "getting trace parent id");
    LOG_TRITONSERVER_ERROR(
        TRITONSERVER_InferenceTraceRequestId(trace, &request_id),
        "getting request id");

    if (ts->setting_->mode_ == TRACE_MODE_TRITON) {
      *ss << "{\"id\":" << id << ",\"model_name\":\"" << model_name
          << "\",\"model_version\":" << model_version;

      if (std::string(request_id) != "") {
        *ss << ",\"request_id\":\"" << request_id << "\"";
      }

      if (parent_id != 0) {
        *ss << ",\"parent_id\":" << parent_id;
      }
      *ss << "},";
    } else if (ts->setting_->mode_ == TRACE_MODE_OPENTELEMETRY) {
#ifndef _WIN32
      if (ts->trace_span_ == nullptr) {
        ts->InitSpan(ts->time_offset_ + std::chrono::nanoseconds{timestamp_ns});
      }
      ts->trace_span_->SetAttribute("triton.model_name", model_name);
      ts->trace_span_->SetAttribute("triton.model_version", model_version);
      ts->trace_span_->SetAttribute("triton.trace_parent_id", parent_id);
      ts->trace_span_->SetAttribute("triton.trace_request_id", request_id);
#else
      LOG_ERROR << "Unsupported trace mode: "
               << TraceManager::InferenceTraceModeString(ts->setting_->mode_);
#endif
    }
  }

  if (ts->setting_->mode_ == TRACE_MODE_TRITON) {
    *ss << "{\"id\":" << id << ",\"timestamps\":["
        << "{\"name\":\"" << TRITONSERVER_InferenceTraceActivityString(activity)
        << "\",\"ns\":" << timestamp_ns << "}]}";
  } else if (ts->setting_->mode_ == TRACE_MODE_OPENTELEMETRY) {
#ifndef _WIN32
    otel_common::SystemTimestamp otel_timestamp{
        (ts->time_offset_ + std::chrono::nanoseconds{timestamp_ns})};
    if (ts->trace_span_ == nullptr) {
      ts->InitSpan(otel_timestamp);
    }
    ts->trace_span_->AddEvent(
        TRITONSERVER_InferenceTraceActivityString(activity), otel_timestamp,
        {{"triton.steady_timestamp_ns", timestamp_ns}});
#else
    LOG_ERROR << "Unsupported trace mode: "
             << TraceManager::InferenceTraceModeString(ts->setting_->mode_);
#endif
  }
}

void
TraceManager::TraceTensorActivity(
    TRITONSERVER_InferenceTrace* trace,
    TRITONSERVER_InferenceTraceActivity activity, const char* name,
    TRITONSERVER_DataType datatype, const void* base, size_t byte_size,
    const int64_t* shape, uint64_t dim_count,
    TRITONSERVER_MemoryType memory_type, int64_t memory_type_id, void* userp)
{
  if ((activity != TRITONSERVER_TRACE_TENSOR_QUEUE_INPUT) &&
      (activity != TRITONSERVER_TRACE_TENSOR_BACKEND_INPUT) &&
      (activity != TRITONSERVER_TRACE_TENSOR_BACKEND_OUTPUT)) {
    LOG_ERROR << "Unsupported activity: "
              << TRITONSERVER_InferenceTraceActivityString(activity);
    return;
  }

  void* buffer_base = const_cast<void*>(base);
  if (memory_type == TRITONSERVER_MEMORY_GPU) {
#ifdef TRITON_ENABLE_GPU
    buffer_base = malloc(byte_size);
    if (buffer_base == nullptr) {
      LOG_ERROR << "Failed to malloc CPU buffer";
      return;
    }
    FAIL_IF_CUDA_ERR(
        cudaMemcpy(buffer_base, base, byte_size, cudaMemcpyDeviceToHost),
        "copying buffer into CPU memory");
#else
    LOG_ERROR << "GPU buffer is unsupported";
    return;
#endif  // TRITON_ENABLE_GPU
  }

  uint64_t id;
  LOG_TRITONSERVER_ERROR(
      TRITONSERVER_InferenceTraceId(trace, &id), "getting trace id");

  // The function may be called with different traces but the same 'userp',
  // group the activity of the same trace together for more readable output.
  auto ts =
      reinterpret_cast<std::shared_ptr<TraceManager::Trace>*>(userp)->get();

  if (ts->setting_->mode_ == TRACE_MODE_OPENTELEMETRY) {
    LOG_ERROR << "Tensor level tracing is not supported by the mode: "
              << TraceManager::InferenceTraceModeString(ts->setting_->mode_);
  } else if (ts->setting_->mode_ == TRACE_MODE_TRITON) {
    std::lock_guard<std::mutex> lk(ts->mtx_);
    std::stringstream* ss = nullptr;
    {
      if (ts->streams_.find(id) == ts->streams_.end()) {
        std::unique_ptr<std::stringstream> stream(new std::stringstream());
        ss = stream.get();
        ts->streams_.emplace(id, std::move(stream));
      } else {
        ss = ts->streams_[id].get();
        // If the string stream is not newly created, add "," as there is
        // already content in the string stream
        *ss << ",";
      }
    }

    // collect and serialize trace details.
    *ss << "{\"id\":" << id << ",\"activity\":\""
        << TRITONSERVER_InferenceTraceActivityString(activity) << "\"";
    // collect tensor
    *ss << ",\"tensor\":{";
    // collect tensor name
    *ss << "\"name\":\"" << std::string(name) << "\"";
    // collect tensor data
    *ss << ",\"data\":\"";
    size_t element_count = 1;
    for (uint64_t i = 0; i < dim_count; i++) {
      element_count *= shape[i];
    }
    switch (datatype) {
      case TRITONSERVER_TYPE_BOOL: {
        const uint8_t* bool_base =
            reinterpret_cast<const uint8_t*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << ((bool_base[e] == 0) ? false : true);
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_UINT8: {
        const uint8_t* cbase = reinterpret_cast<const uint8_t*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_UINT16: {
        const uint16_t* cbase = reinterpret_cast<const uint16_t*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_UINT32: {
        const uint32_t* cbase = reinterpret_cast<const uint32_t*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_UINT64: {
        const uint64_t* cbase = reinterpret_cast<const uint64_t*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_INT8: {
        const int8_t* cbase = reinterpret_cast<const int8_t*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_INT16: {
        const int16_t* cbase = reinterpret_cast<const int16_t*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_INT32: {
        const int32_t* cbase = reinterpret_cast<const int32_t*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_INT64: {
        const int64_t* cbase = reinterpret_cast<const int64_t*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }

      // FP16 / BF16 already handled as binary blobs, no need to manipulate
      // here
      case TRITONSERVER_TYPE_FP16: {
        break;
      }
      case TRITONSERVER_TYPE_BF16: {
        break;
      }

      case TRITONSERVER_TYPE_FP32: {
        const float* cbase = reinterpret_cast<const float*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_FP64: {
        const double* cbase = reinterpret_cast<const double*>(buffer_base);
        for (size_t e = 0; e < element_count; ++e) {
          *ss << cbase[e];
          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_BYTES: {
        const char* cbase = reinterpret_cast<const char*>(buffer_base);
        size_t offset = 0;
        for (size_t e = 0; e < element_count; ++e) {
          if ((offset + sizeof(uint32_t)) > byte_size) {
            return;
          }
          const size_t len =
              *(reinterpret_cast<const uint32_t*>(cbase + offset));
          offset += sizeof(uint32_t);
          if ((offset + len) > byte_size) {
            return;
          }
          std::string str(cbase + offset, len);
          *ss << "\\\"" << str << "\\\"";
          offset += len;

          if (e < (element_count - 1))
            *ss << ",";
        }
        break;
      }
      case TRITONSERVER_TYPE_INVALID: {
        return;
      }
    }
    *ss << "\",\"shape\":\"";
    for (uint64_t i = 0; i < dim_count; i++) {
      *ss << shape[i];
      if (i < (dim_count - 1)) {
        *ss << ",";
      }
    }
    *ss << "\",\"dtype\":\"" << TRITONSERVER_DataTypeString(datatype) << "\"}";
    *ss << "}";
  }

  if (memory_type == TRITONSERVER_MEMORY_GPU) {
#ifdef TRITON_ENABLE_GPU
    if (buffer_base != nullptr) {
      free(buffer_base);
    }
#endif  // TRITON_ENABLE_GPU
  }
}

TraceManager::TraceFile::~TraceFile()
{
  if (!first_write_) {
    trace_file_ << "]";
  }
}

void
TraceManager::TraceFile::SaveTraces(
    std::stringstream& trace_stream, const bool to_index_file)
{
  try {
    if (to_index_file) {
      std::string file_name =
          file_name_ + "." + std::to_string(index_.fetch_add(1));
      std::ofstream file_stream;
      file_stream.open(file_name);
      file_stream << "[";
      file_stream << trace_stream.rdbuf();
      file_stream << "]";
    } else {
      std::lock_guard<std::mutex> lock(mu_);
      if (first_write_) {
        trace_file_.open(file_name_);
        trace_file_ << "[";
        first_write_ = false;
      } else {
        trace_file_ << ",";
      }
      trace_file_ << trace_stream.rdbuf();
    }
  }
  catch (const std::ofstream::failure& e) {
    LOG_ERROR << "failed creating trace file: " << e.what();
  }
  catch (...) {
    LOG_ERROR << "failed creating trace file: reason unknown";
  }
}

std::shared_ptr<TraceManager::Trace>
TraceManager::TraceSetting::SampleTrace()
{
  bool create_trace = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!Valid()) {
      return nullptr;
    }
    create_trace = (((++sample_) % rate_) == 0);
    if (create_trace && (count_ > 0)) {
      --count_;
      ++created_;
    }
  }
  if (create_trace) {
    std::shared_ptr<TraceManager::Trace> lts(new Trace());
    // Split 'Trace' management to frontend and Triton trace separately
    // to avoid dependency between frontend request and Triton trace's
    // liveness
    auto trace_userp = new std::shared_ptr<TraceManager::Trace>(lts);
    TRITONSERVER_InferenceTrace* trace;
    TRITONSERVER_Error* err = TRITONSERVER_InferenceTraceTensorNew(
        &trace, level_, 0 /* parent_id */, TraceActivity, TraceTensorActivity,
        TraceRelease, trace_userp);
    if (err != nullptr) {
      LOG_TRITONSERVER_ERROR(err, "creating inference trace object");
      delete trace_userp;
      return nullptr;
    }
    lts->trace_ = trace;
    lts->trace_userp_ = trace_userp;
    LOG_TRITONSERVER_ERROR(
        TRITONSERVER_InferenceTraceId(trace, &lts->trace_id_),
        "getting trace id");
    if (mode_ == TRACE_MODE_OPENTELEMETRY) {
#ifndef _WIN32
      lts->InitTracer(config_map_);
#else
      LOG_ERROR << "Unsupported trace mode: "
               << TraceManager::InferenceTraceModeString(mode_);
#endif
    }
    return lts;
  }
  return nullptr;
}

void
TraceManager::TraceSetting::WriteTrace(
    const std::unordered_map<uint64_t, std::unique_ptr<std::stringstream>>&
        streams)
{
  std::unique_lock<std::mutex> lock(mu_);

  if (sample_in_stream_ != 0) {
    trace_stream_ << ",";
  }
  ++sample_in_stream_;
  ++collected_;

  size_t stream_count = 0;
  for (const auto& stream : streams) {
    trace_stream_ << stream.second->rdbuf();
    // Need to add ',' unless it is the last trace in the group
    ++stream_count;
    if (stream_count != streams.size()) {
      trace_stream_ << ",";
    }
  }
  // Write to file with index when one of the following is true
  // 1. trace_count is specified and that number of traces has been collected
  // 2. log_frequency is specified and that number of traces has been
  // collected
  if (((count_ == 0) && (collected_ == sample_)) ||
      ((log_frequency_ != 0) && (sample_in_stream_ >= log_frequency_))) {
    // Reset variables and release lock before saving to file
    sample_in_stream_ = 0;
    std::stringstream stream;
    trace_stream_.swap(stream);
    lock.unlock();

    file_->SaveTraces(stream, true /* to_index_file */);
  }
}

TraceManager::TraceSetting::TraceSetting(
    const TRITONSERVER_InferenceTraceLevel level, const uint32_t rate,
    const int32_t count, const uint32_t log_frequency,
    const std::shared_ptr<TraceFile>& file, const InferenceTraceMode mode,
    const TraceConfigMap& config_map, const bool level_specified,
    const bool rate_specified, const bool count_specified,
    const bool log_frequency_specified, const bool filepath_specified,
    const bool mode_specified, const bool config_map_specified)
    : level_(level), rate_(rate), count_(count), log_frequency_(log_frequency),
      file_(file), mode_(mode), config_map_(config_map),
      level_specified_(level_specified), rate_specified_(rate_specified),
      count_specified_(count_specified),
      log_frequency_specified_(log_frequency_specified),
      filepath_specified_(filepath_specified), mode_specified_(mode_specified),
      config_map_specified_(config_map_specified), sample_(0), created_(0),
      collected_(0), sample_in_stream_(0)
{
  if (level_ == TRITONSERVER_TRACE_LEVEL_DISABLED) {
    invalid_reason_ = "tracing is disabled";
  } else if (rate_ == 0) {
    invalid_reason_ = "sample rate must be non-zero";
  } else if (mode_ == TRACE_MODE_TRITON && file_->FileName().empty()) {
    invalid_reason_ = "trace file name is not given";
  }
}

TraceManager::TraceSetting::~TraceSetting()
{
  // If log frequency is set, should log the remaining traces to indexed file.
  if (mode_ == TRACE_MODE_TRITON && sample_in_stream_ != 0) {
    file_->SaveTraces(trace_stream_, (log_frequency_ != 0));
  }
}
}}  // namespace triton::server
