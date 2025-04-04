/* Copyright 2020 The MLPerf Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "flutter/cpp/backends/external.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "flutter/cpp/backend.h"
#include "flutter/cpp/utils.h"
#include "tensorflow/lite/shared_library.h"

namespace mlperf {
namespace mobile {

static std::string pbdataStr;

std::string BackendFunctions::isSupported(const std::string& lib_path,
                                          const std::string& native_lib_path,
                                          const std::string& manufacturer,
                                          const std::string& model,
                                          const char** pbdata) {
  const char* msg = nullptr;
  *pbdata = nullptr;
  BackendFunctions backend(lib_path);
  if (backend.isLoaded()) {
    mlperf_device_info_t device_info{model.c_str(), manufacturer.c_str(),
                                     native_lib_path.c_str()};
    bool match = backend.match(&msg, pbdata, &device_info);
    if (match) {
      pbdataStr = *pbdata;
      *pbdata = pbdataStr.c_str();
    }

    if (!match) {
      // If the device is not supported. Error out.
      msg = "UnsupportedSoc";
    } else if (msg == nullptr) {
      // If the device is known and supported.
      msg = "Supported";
      pbdataStr = *pbdata;
      *pbdata = pbdataStr.c_str();
    } else {
      // If the device is known but not supported.
      msg = "Unsupported";
    }
  } else {
    // If the backend couldn't be loaded try to load next lib
    msg = "UnsupportedSoc";
  }
  return std::string(msg);
}

#if defined(_WIN64) || defined(_WIN32)
static std::wstring getExecutableDirectory() {
  uint64_t bufferSize = 300;
  std::wstring curDir;
  while (true) {
    curDir.resize(bufferSize);
    uint64_t pathLength =
        GetModuleFileNameW(nullptr, &curDir[0], curDir.size());
    const auto errorCode = GetLastError();
    if (errorCode == ERROR_SUCCESS) {
      break;
    }
    if (errorCode == ERROR_INSUFFICIENT_BUFFER) {
      bufferSize *= 2;
      continue;
    }
    LOG(ERROR) << "Unable to get .exe file path (error  " << errorCode
               << "): " << std::system_category().message(errorCode);
    return {};
  }
  curDir = curDir.substr(0, curDir.find_last_of(L"\\/") + 1);
  return curDir;
}

static void* loadBackendDll(const std::string& lib_path) {
  std::wstring wide_lib_path;
  wide_lib_path.resize(lib_path.size() + 1);
  std::mbstate_t state{};
  const char* src = lib_path.c_str();
  auto conv_res =
      std::mbsrtowcs(&wide_lib_path[0], &src, wide_lib_path.size(), &state);
  if (conv_res == std::string::npos) {
    LOG(ERROR) << "Unable to load: " << lib_path
               << ": can't convert to std::wstring";
    return nullptr;
  }

  std::wstring curDir = getExecutableDirectory();
  if (curDir.empty()) {
    return nullptr;
  }
  const wchar_t* libsFolderPath = L"libs";
  const std::wstring libFolder = std::wstring{curDir} + libsFolderPath;
  const auto setDllResult = SetDllDirectoryW(libFolder.c_str());
  if (setDllResult == 0) {
    const auto errorCode = GetLastError();
    LOG(ERROR) << "Unable to add './libs' as a library path (error  "
               << errorCode
               << "): " << std::system_category().message(errorCode);
    return nullptr;
  }

  auto handle = tflite::SharedLibrary::LoadLibrary(wide_lib_path.c_str());
  if (handle == nullptr) {
    const auto errorCode = GetLastError();
    LOG(ERROR) << "Can't load library in path '" << lib_path << "' (error  "
               << errorCode
               << "): " << std::system_category().message(errorCode);
  }
  return handle;
}
#endif

BackendFunctions::BackendFunctions(const std::string& lib_path) {
  if (!lib_path.empty()) {
    isloaded = false;
#if defined(_WIN64) || defined(_WIN32)
    handle = loadBackendDll(lib_path);
#else
    handle = tflite::SharedLibrary::LoadLibrary(lib_path.c_str());
    if (handle == nullptr) {
      LOG(ERROR) << "Unable to load: " << lib_path << ": "
                 << tflite::SharedLibrary::GetError();
    }
#endif
    if (handle == nullptr) {
      return;
    }
    isloaded = true;
  }

  match = reinterpret_cast<decltype(match)>(
      GetSymbol("mlperf_backend_matches_hardware"));
  create =
      reinterpret_cast<decltype(create)>(GetSymbol("mlperf_backend_create"));
  backend_name = reinterpret_cast<decltype(backend_name)>(
      GetSymbol("mlperf_backend_name"));
  vendor = reinterpret_cast<decltype(vendor)>(
      GetSymbol("mlperf_backend_vendor_name"));
  accelerator_name = reinterpret_cast<decltype(accelerator_name)>(
      GetSymbol("mlperf_backend_accelerator_name"));
  destroy =
      reinterpret_cast<decltype(destroy)>(GetSymbol("mlperf_backend_delete"));

  issue_query = reinterpret_cast<decltype(issue_query)>(
      GetSymbol("mlperf_backend_issue_query"));
  flush_queries = reinterpret_cast<decltype(flush_queries)>(
      GetSymbol("mlperf_backend_flush_queries"));

  get_input_count = reinterpret_cast<decltype(get_input_count)>(
      GetSymbol("mlperf_backend_get_input_count"));
  get_input_type = reinterpret_cast<decltype(get_input_type)>(
      GetSymbol("mlperf_backend_get_input_type"));
  set_input = reinterpret_cast<decltype(set_input)>(
      GetSymbol("mlperf_backend_set_input"));

  get_output_count = reinterpret_cast<decltype(get_output_count)>(
      GetSymbol("mlperf_backend_get_output_count"));
  get_output_type = reinterpret_cast<decltype(get_output_type)>(
      GetSymbol("mlperf_backend_get_output_type"));
  get_output = reinterpret_cast<decltype(get_output)>(
      GetSymbol("mlperf_backend_get_output"));

  // Backends do not have to define an allocator, but use it if they do
  get_buffer = reinterpret_cast<decltype(get_buffer)>(
      CheckSymbol("mlperf_backend_get_buffer"));
  release_buffer = reinterpret_cast<decltype(release_buffer)>(
      CheckSymbol("mlperf_backend_release_buffer"));

  // Backends may need to change the format of the inputs (e.g. channel order)
  convert_inputs = reinterpret_cast<decltype(convert_inputs)>(
      CheckSymbol("mlperf_backend_convert_inputs"));
  // Backends may need to change the format of the outputs (e.g. channel order)
  convert_outputs = reinterpret_cast<decltype(convert_outputs)>(
      CheckSymbol("mlperf_backend_convert_outputs"));
  // If both functions are defined, then update
  if (get_buffer && release_buffer) {
    LOG(INFO) << "Using backend allocator";
    AllocatorMgr::UseBackendAllocator(get_buffer, release_buffer);
  } else {
    AllocatorMgr::UseDefaultAllocator();
    LOG(INFO) << "Using default allocator";
  }
}

ExternalBackend::ExternalBackend(const std::string& model_file_path,
                                 const std::string& lib_path,
                                 const SettingList& settings,
                                 const std::string& native_lib_path)
    : backend_functions_(lib_path) {
  backend_config_ = CppToCSettings(settings);
  backend_ptr_ = backend_functions_.create(
      model_file_path.c_str(), &backend_config_, native_lib_path.c_str());
  if (!backend_ptr_) {
    LOG(FATAL) << "Failed to create external backend";
  }

  // Get input and output types.
  int32_t input_count = backend_functions_.get_input_count(backend_ptr_);
  for (int i = 0; i < input_count; ++i) {
    input_format_.push_back(backend_functions_.get_input_type(backend_ptr_, i));
  }

  int32_t ouput_count = backend_functions_.get_output_count(backend_ptr_);
  for (int i = 0; i < ouput_count; ++i) {
    output_format_.push_back(
        backend_functions_.get_output_type(backend_ptr_, i));
  }

  backend_name_ = backend_functions_.backend_name(backend_ptr_);
  vendor_ = backend_functions_.vendor(backend_ptr_);
  accelerator_name_ = backend_functions_.accelerator_name(backend_ptr_);
}

}  // namespace mobile
}  // namespace mlperf
