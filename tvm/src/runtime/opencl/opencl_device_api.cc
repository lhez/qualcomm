/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file opencl_device_api.cc
 */
#include <dmlc/thread_local.h>
#include <tvm/runtime/registry.h>

#include "opencl_common.h"

namespace tvm {
namespace runtime {
namespace cl {

namespace {
std::tuple<size_t, size_t> GetImageInfo(const void* mem_ptr, size_t* origin, size_t* region) {
  cl_mem mem = static_cast<cl_mem>((void*)mem_ptr);
  size_t width, height;
  OPENCL_CALL(clGetImageInfo(mem, CL_IMAGE_WIDTH, sizeof(width), &width, NULL));
  OPENCL_CALL(clGetImageInfo(mem, CL_IMAGE_HEIGHT, sizeof(height), &height, NULL));
  // Current support is for image2d only
  size_t depth = 1;
  // OPENCL_CALL(clGetImageInfo(mem, CL_IMAGE_DEPTH, sizeof(depth), &depth, NULL));
  region[0] = width;
  region[1] = height;
  region[2] = depth;
  origin[0] = 0;
  origin[1] = 0;
  origin[2] = 0;
  // return row_pitch == slice_pitch == 0
  return std::make_tuple(0 , 0);
}
}

OpenCLBuffer::MemoryLayout OpenCLBuffer::MemoryLayoutFromScope(Optional<String> mem_scope) {
  if (!mem_scope.defined()) {
    return OpenCLBuffer::MemoryLayout::kGlobalRowMajor;
  } else if (mem_scope.value() == "texture") {
    return OpenCLBuffer::MemoryLayout::kTexture2DActivation;
  } else if (mem_scope.value() == "texture:weight") {
    return OpenCLBuffer::MemoryLayout::kTexture2DWeight;
  } else if (mem_scope.value() == "texture:nhwc") {
    return OpenCLBuffer::MemoryLayout::kTexture2DNHWC;
  }
  LOG(FATAL) << "No memory layout defined for memory of scope: " << mem_scope.value();
  return OpenCLBuffer::MemoryLayout::kUndefined;
}

OpenCLThreadEntry* OpenCLWorkspace::GetThreadEntry() { return OpenCLThreadEntry::ThreadLocal(); }

OpenCLWorkspace* OpenCLWorkspace::Global() {
  static OpenCLWorkspace* inst = new OpenCLWorkspace();
  return inst;
}

void OpenCLWorkspace::SetDevice(TVMContext ctx) {
  GetThreadEntry()->context.device_id = ctx.device_id;
}

void OpenCLWorkspace::GetAttr(TVMContext ctx, DeviceAttrKind kind, TVMRetValue* rv) {
  this->Init();
  size_t index = static_cast<size_t>(ctx.device_id);
  if (kind == kExist) {
    *rv = static_cast<int>(index < devices.size());
    return;
  }
  ICHECK_LT(index, devices.size()) << "Invalid device id " << index;
  switch (kind) {
    case kExist:
      break;
    case kMaxThreadsPerBlock: {
      size_t value;
      OPENCL_CALL(clGetDeviceInfo(devices[index], CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(size_t),
                                  &value, nullptr));
      *rv = static_cast<int64_t>(value);
      break;
    }
    case kWarpSize: {
      /* TODO: the warp size of OpenCL device is not always 1
               e.g. Intel Graphics has a sub group concept which contains 8 - 32 work items,
               corresponding to the number of SIMD entries the heardware configures.
               We need to figure out a way to query this information from the hardware.
      */
      *rv = 1;
      break;
    }
    case kMaxSharedMemoryPerBlock: {
      cl_ulong value;
      OPENCL_CALL(clGetDeviceInfo(devices[index], CL_DEVICE_LOCAL_MEM_SIZE, sizeof(cl_ulong),
                                  &value, nullptr));
      *rv = static_cast<int64_t>(value);
      break;
    }
    case kComputeVersion:
      return;
    case kDeviceName: {
      char value[128] = {0};
      OPENCL_CALL(
          clGetDeviceInfo(devices[index], CL_DEVICE_NAME, sizeof(value) - 1, value, nullptr));
      *rv = std::string(value);
      break;
    }
    case kMaxClockRate: {
      cl_uint value;
      OPENCL_CALL(clGetDeviceInfo(devices[index], CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(cl_uint),
                                  &value, nullptr));
      *rv = static_cast<int32_t>(value);
      break;
    }
    case kMultiProcessorCount: {
      cl_uint value;
      OPENCL_CALL(clGetDeviceInfo(devices[index], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cl_uint),
                                  &value, nullptr));
      *rv = static_cast<int32_t>(value);
      break;
    }
    case kMaxThreadDimensions: {
      size_t dims[3];
      OPENCL_CALL(clGetDeviceInfo(devices[index], CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(dims), dims,
                                  nullptr));

      std::stringstream ss;  // use json string to return multiple int values;
      ss << "[" << dims[0] << ", " << dims[1] << ", " << dims[2] << "]";
      *rv = ss.str();
      break;
    }
    case kMaxRegistersPerBlock:
      return;
    case kGcnArch:
      return;
    case kApiVersion:
      return;
  }
}

void* OpenCLWorkspace::AllocDataSpace(TVMContext ctx, size_t size, size_t alignment,
                                      DLDataType type_hint) {
  this->Init();
  ICHECK(context != nullptr) << "No OpenCL device";
  cl_int err_code;
  OpenCLBuffer* mptr = new OpenCLBuffer();
  mptr->buffer = clCreateBuffer(this->context, CL_MEM_READ_WRITE, size, nullptr, &err_code);
  mptr->layout = OpenCLBuffer::MemoryLayout::kGlobalRowMajor;
  OPENCL_CHECK_ERROR(err_code);
  return mptr;
}

void* OpenCLWorkspace::AllocDataSpace(TVMContext ctx, int ndim, const int64_t* shape, DLDataType dtype,
                                      Optional<String> mem_scope) {
  if (!mem_scope.defined() || mem_scope.value() == "global") {
    return DeviceAPI::AllocDataSpace(ctx, ndim, shape, dtype, mem_scope);
  }
  ICHECK(IsTextureStorage(std::string(mem_scope.value())))
    << "Device does not support allocate data space with "
    << "specified memory scope: " << mem_scope.value();

  ICHECK(ndim > 2) << "Shape for texture allocation must be at least rank 3; "
                   << "provided shape is rank " << ndim;

  OpenCLBuffer* mptr = new OpenCLBuffer(mem_scope);
  size_t axis = DefaultTextureLayoutSeparator(ndim, mem_scope.value());
  auto texture = ApplyTexture2DFlattening<int64_t>(shape, ndim, axis);
  mptr->buffer = AllocTexture(ctx, texture.width, texture.height, dtype);
  return mptr;
}

void OpenCLWorkspace::FreeDataSpace(TVMContext ctx, void* ptr) {
  // We have to make sure that the memory object is not in the command queue
  // for some OpenCL platforms.
  OPENCL_CALL(clFinish(this->GetQueue(ctx)));

  OpenCLBuffer* mptr = static_cast<OpenCLBuffer*>(ptr);
  OPENCL_CALL(clReleaseMemObject(mptr->buffer));
  delete mptr;
}

cl_mem OpenCLWorkspace::AllocTexture(TVMContext ctx, size_t width, size_t height, DLDataType type_hint) {
  this->Init();
  ICHECK(context != nullptr) << "No OpenCL device";
  cl_int err_code;
  cl_channel_type cl_type = DTypeToOpenCLChannelType(type_hint);
  cl_image_format format = { CL_RGBA, cl_type };
  cl_image_desc descriptor = { CL_MEM_OBJECT_IMAGE2D, width, height, 0, 0, 0, 0, 0, 0 };
  cl_mem mptr = clCreateImage(
    this->context,
    CL_MEM_READ_WRITE,
    &format,
    &descriptor,
    nullptr,
    &err_code);
  OPENCL_CHECK_ERROR(err_code);
  return mptr;
}

void* OpenCLWorkspace::AllocTextureWorkspace(TVMContext ctx, size_t width, size_t height, DLDataType type_hint) {
  return GetThreadEntry()->texture_pool.AllocTexture(ctx, width, height, type_hint);
}

void OpenCLWorkspace::FreeTextureWorkspace(TVMContext ctx, void* ptr) {
  GetThreadEntry()->texture_pool.FreeTexture(ctx, ptr);
}

void OpenCLWorkspace::CopyDataFromTo(const void* from, size_t from_offset, void* to,
                                     size_t to_offset, size_t size, TVMContext ctx_from,
                                     TVMContext ctx_to, DLDataType type_hint,
                                     TVMStreamHandle stream) {
  this->Init();
  ICHECK(stream == nullptr);
  if (IsOpenCLDevice(ctx_from) && IsOpenCLDevice(ctx_to)) {
    const auto* from_buf = static_cast<const OpenCLBuffer*>(from);
    auto* to_buf = static_cast<OpenCLBuffer*>(to);
    OPENCL_CALL(clEnqueueCopyBuffer(this->GetQueue(ctx_to), from_buf->buffer, to_buf->buffer,
                                    from_offset, to_offset, size, 0, nullptr, nullptr));
  } else if (IsOpenCLDevice(ctx_from) && ctx_to.device_type == kDLCPU) {
    const auto* from_buf = static_cast<const OpenCLBuffer*>(from);
    cl_mem_object_type from_type = GetMemObjectType(from_buf->buffer);
    switch (from_type) {
    case CL_MEM_OBJECT_BUFFER:
      OPENCL_CALL(clEnqueueReadBuffer(this->GetQueue(ctx_from), from_buf->buffer, CL_FALSE,
                                      from_offset, size, static_cast<char*>(to) + to_offset, 0,
                                      nullptr, nullptr));
      break;
    case CL_MEM_OBJECT_IMAGE2D:
      size_t origin[3], region[3];
      size_t row_pitch, slice_pitch;
      std::tie(row_pitch, slice_pitch) = GetImageInfo(from_buf->buffer, origin, region);
      // TODO(csullivan): Support calculating row_pitch correctly in the case of reuse.
      // Note that when utilizing texture pools for memory reuse, the allocated image
      // size can be larger than the size to be read.
      OPENCL_CALL(clEnqueueReadImage(this->GetQueue(ctx_from), from_buf->buffer, CL_FALSE, origin,
                                     region, row_pitch, slice_pitch,
                                     static_cast<char*>(to) + to_offset, 0, nullptr, nullptr));
      break;
    default:
      LOG(FATAL) << "Device storage transfer from cl_mem_object_type: " << from_type
                 << " to host memory is not yet supported";
    }
    OPENCL_CALL(clFinish(this->GetQueue(ctx_from)));
  } else if (ctx_from.device_type == kDLCPU && IsOpenCLDevice(ctx_to)) {
    auto* to_buf = static_cast<OpenCLBuffer*>(to);
    cl_mem_object_type to_type = GetMemObjectType(to_buf->buffer);
    switch (to_type) {
    case CL_MEM_OBJECT_BUFFER:
      OPENCL_CALL(clEnqueueWriteBuffer(this->GetQueue(ctx_to), to_buf->buffer, CL_FALSE, to_offset,
                                       size, static_cast<const char*>(from) + from_offset, 0,
                                       nullptr, nullptr));
      break;
    case CL_MEM_OBJECT_IMAGE2D:
      size_t origin[3], region[3];
      size_t row_pitch, slice_pitch;
      std::tie(row_pitch, slice_pitch) = GetImageInfo(to_buf->buffer, origin, region);
      OPENCL_CALL(clEnqueueWriteImage(
          this->GetQueue(ctx_to), to_buf->buffer, CL_FALSE, origin, region, row_pitch, slice_pitch,
          static_cast<const char*>(from) + from_offset, 0, nullptr, nullptr));
      break;
    default:
      LOG(FATAL) << "Device storage transfer from host memory to cl_mem_object_type: " << to_type
                 << " is not yet supported";
    }
    OPENCL_CALL(clFinish(this->GetQueue(ctx_to)));
  } else {
    LOG(FATAL) << "Expect copy from/to OpenCL or between OpenCL";
  }
}

void OpenCLWorkspace::StreamSync(TVMContext ctx, TVMStreamHandle stream) {
  ICHECK(stream == nullptr);
  OPENCL_CALL(clFinish(this->GetQueue(ctx)));
}

void* OpenCLWorkspace::AllocWorkspace(TVMContext ctx, size_t size, DLDataType type_hint) {
  return GetThreadEntry()->pool.AllocWorkspace(ctx, size);
}

void OpenCLWorkspace::FreeWorkspace(TVMContext ctx, void* data) {
  GetThreadEntry()->pool.FreeWorkspace(ctx, data);
}

typedef dmlc::ThreadLocalStore<OpenCLThreadEntry> OpenCLThreadStore;

OpenCLThreadEntry* OpenCLThreadEntry::ThreadLocal() { return OpenCLThreadStore::Get(); }

std::string GetPlatformInfo(cl_platform_id pid, cl_platform_info param_name) {
  size_t ret_size;
  OPENCL_CALL(clGetPlatformInfo(pid, param_name, 0, nullptr, &ret_size));
  std::string ret;
  ret.resize(ret_size);
  OPENCL_CALL(clGetPlatformInfo(pid, param_name, ret_size, &ret[0], nullptr));
  return ret;
}

std::string GetDeviceInfo(cl_device_id pid, cl_device_info param_name) {
  size_t ret_size;
  OPENCL_CALL(clGetDeviceInfo(pid, param_name, 0, nullptr, &ret_size));
  std::string ret;
  ret.resize(ret_size);
  OPENCL_CALL(clGetDeviceInfo(pid, param_name, ret_size, &ret[0], nullptr));
  return ret;
}

std::vector<cl_platform_id> GetPlatformIDs() {
  cl_uint ret_size;
  cl_int code = clGetPlatformIDs(0, nullptr, &ret_size);
  std::vector<cl_platform_id> ret;
  if (code != CL_SUCCESS) return ret;
  ret.resize(ret_size);
  OPENCL_CALL(clGetPlatformIDs(ret_size, &ret[0], nullptr));
  return ret;
}

std::vector<cl_device_id> GetDeviceIDs(cl_platform_id pid, std::string device_type) {
  cl_device_type dtype = CL_DEVICE_TYPE_ALL;
  if (device_type == "cpu") dtype = CL_DEVICE_TYPE_CPU;
  if (device_type == "gpu") dtype = CL_DEVICE_TYPE_GPU;
  if (device_type == "accelerator") dtype = CL_DEVICE_TYPE_ACCELERATOR;
  cl_uint ret_size;
  cl_int code = clGetDeviceIDs(pid, dtype, 0, nullptr, &ret_size);
  std::vector<cl_device_id> ret;
  if (code != CL_SUCCESS) return ret;
  ret.resize(ret_size);
  OPENCL_CALL(clGetDeviceIDs(pid, dtype, ret_size, &ret[0], nullptr));
  return ret;
}

bool MatchPlatformInfo(cl_platform_id pid, cl_platform_info param_name, std::string value) {
  if (value.length() == 0) return true;
  std::string param_value = GetPlatformInfo(pid, param_name);
  return param_value.find(value) != std::string::npos;
}

void OpenCLWorkspace::Init(const std::string& type_key, const std::string& device_type,
                           const std::string& platform_name) {
  if (initialized_) return;
  std::lock_guard<std::mutex> lock(this->mu);
  if (initialized_) return;
  if (context != nullptr) return;
  this->type_key = type_key;
  // matched platforms
  std::vector<cl_platform_id> platform_ids = cl::GetPlatformIDs();
  if (platform_ids.size() == 0) {
    LOG(WARNING) << "No OpenCL platform matched given existing options ...";
    return;
  }
  this->platform_id = nullptr;
  for (auto platform_id : platform_ids) {
    if (!MatchPlatformInfo(platform_id, CL_PLATFORM_NAME, platform_name)) {
      continue;
    }
    std::vector<cl_device_id> devices_matched = cl::GetDeviceIDs(platform_id, device_type);
    if ((devices_matched.size() == 0) && (device_type == "gpu")) {
      LOG(WARNING) << "Using CPU OpenCL device";
      devices_matched = cl::GetDeviceIDs(platform_id, "cpu");
    }
    if (devices_matched.size() > 0) {
      this->platform_id = platform_id;
      this->platform_name = cl::GetPlatformInfo(platform_id, CL_PLATFORM_NAME);
      this->device_type = device_type;
      this->devices = devices_matched;
      break;
    }
  }
  if (this->platform_id == nullptr) {
    LOG(WARNING) << "No OpenCL device";
    return;
  }
  cl_int err_code;
  this->context = clCreateContext(nullptr, this->devices.size(), &(this->devices[0]), nullptr,
                                  nullptr, &err_code);
  OPENCL_CHECK_ERROR(err_code);
  ICHECK_EQ(this->queues.size(), 0U);
  for (size_t i = 0; i < this->devices.size(); ++i) {
    cl_device_id did = this->devices[i];
    this->queues.push_back(clCreateCommandQueue(this->context, did, 0, &err_code));
    OPENCL_CHECK_ERROR(err_code);
  }
  initialized_ = true;
}


TVM_REGISTER_GLOBAL("device_api.opencl.AllocTexture").set_body([](TVMArgs args, TVMRetValue* rv) {
  int device_type = args[0];
  int device_id = args[1];
  int width = args[2];
  int height = args[3];
  int dtype_code_hint = args[4];
  int dtype_bits_hint = args[5];
  TVMContext ctx;
  ctx.device_type = static_cast<DLDeviceType>(device_type);
  ctx.device_id = device_id;

  DLDataType type_hint;
  type_hint.code = static_cast<decltype(type_hint.code)>(dtype_code_hint);
  type_hint.bits = static_cast<decltype(type_hint.bits)>(dtype_bits_hint);
  type_hint.lanes = 1;

  OpenCLWorkspace* ptr = OpenCLWorkspace::Global();
  *rv = ptr->AllocTextureWorkspace(ctx,
                             static_cast<size_t>(width),
                             static_cast<size_t>(height),
                             type_hint);
});

TVM_REGISTER_GLOBAL("device_api.opencl.FreeTexture").set_body([](TVMArgs args, TVMRetValue* rv) {
  int device_type = args[0];
  int device_id = args[1];
  void* data = args[2];
  OpenCLWorkspace* ptr = OpenCLWorkspace::Global();
  TVMContext ctx;
  ctx.device_type = static_cast<DLDeviceType>(device_type);
  ctx.device_id = device_id;
  ptr->FreeTextureWorkspace(ctx, data);
  *rv = static_cast<int32_t>(0);
});

TVM_REGISTER_GLOBAL("device_api.opencl").set_body([](TVMArgs args, TVMRetValue* rv) {
  DeviceAPI* ptr = OpenCLWorkspace::Global();
  *rv = static_cast<void*>(ptr);
});

}  // namespace cl
}  // namespace runtime
}  // namespace tvm
