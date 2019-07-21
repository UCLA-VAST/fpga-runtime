#ifndef FPGA_RUNTIME_H_
#define FPGA_RUNTIME_H_

#include <algorithm>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NDEBUG
#include <iostream>
#endif

#define CL_HPP_CL_1_2_DEFAULT_BUILD
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#include <CL/cl2.hpp>

#include "opencl-errors.h"

namespace fpga {

#define CL_CHECK(err) ClCheck((err), __FILE__, __LINE__);
inline void ClCheck(cl_int err, const char* file, int line) {
  if (err != CL_SUCCESS) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": " + ToString(err));
  }
}
#define FUNC_INFO(index)                                  \
  std::clog << "DEBUG: Function ‘" << __PRETTY_FUNCTION__ \
            << "’ called with index = " << (index) << std::endl;

template <typename T>
class Buffer {
  T* ptr_;
  size_t n_;

 public:
  Buffer(T* ptr, size_t n) : ptr_(ptr), n_(n) {}
  operator T*() const { return ptr_; }
  T& operator*() const { return *ptr_; }
  T* operator->() const { return ptr_; }
  T* Get() const { return ptr_; }
  size_t SizeInBytes() const { return n_ * sizeof(T); }
};
template <typename T>
class RoBuf : public Buffer<T> {
  using Buffer<T>::Buffer;
};
template <typename T>
class WoBuf : public Buffer<T> {
  using Buffer<T>::Buffer;
};
template <typename T>
class RwBuf : public Buffer<T> {
  using Buffer<T>::Buffer;
};

template <typename T>
RoBuf<T> ReadOnly(T* ptr, size_t n) {
  return RoBuf<T>(ptr, n);
}
template <typename T>
WoBuf<T> WriteOnly(T* ptr, size_t n) {
  return WoBuf<T>(ptr, n);
}
template <typename T>
RwBuf<T> ReadWrite(T* ptr, size_t n) {
  return RwBuf<T>(ptr, n);
}

class Stream {
 protected:
  const std::string name_;
  cl_stream stream_ = nullptr;

  Stream(const std::string& name) : name_(name) {}
  Stream(const Stream&) = delete;
  ~Stream() {
    if (stream_ != nullptr) {
      CL_CHECK(clReleaseStream(stream_));
    }
  }

  void Attach(const cl::Device& device, const cl::Kernel& kernel, int index,
              cl_stream_flags flags) {
#ifndef NDEBUG
    std::clog << "DEBUG: Stream ‘" << name_ << "’ attached to argument #"
              << index << std::endl;
#endif
    cl_mem_ext_ptr_t ext;
    ext.flags = index;
    ext.param = kernel.get();
    ext.obj = nullptr;
    cl_int err;
    stream_ = clCreateStream(device.get(), flags, CL_STREAM, &ext, &err);
    CL_CHECK(err);
  }
};

class ReadStream : public Stream {
 public:
  using Stream::Attach;
  ReadStream(const std::string& name) : Stream(name) {}
  ReadStream(const ReadStream&) = delete;

  void Attach(const cl::Device& device, const cl::Kernel& kernel, int index) {
    Attach(device, kernel, index, CL_STREAM_READ_ONLY);
  }

  template <typename T>
  void Read(T* host_ptr, uint64_t size, bool eos = true) {
    if (stream_ == nullptr) {
      throw std::runtime_error("cannot read from null stream");
    }
    cl_stream_xfer_req req{0};
    if (eos) {
      req.flags = CL_STREAM_EOT;
    }
    req.priv_data = const_cast<char*>(name_.c_str());
    cl_int err;
    clReadStream(stream_, host_ptr, size * sizeof(T), &req, &err);
    CL_CHECK(err);
  }
};

class WriteStream : public Stream {
 public:
  using Stream::Attach;
  WriteStream(const std::string& name) : Stream(name) {}
  WriteStream(const WriteStream&) = delete;

  void Attach(cl::Device device, cl::Kernel kernel, int index) {
    Attach(device, kernel, index, CL_STREAM_WRITE_ONLY);
  }

  template <typename T>
  void Write(const T* host_ptr, uint64_t size, bool eos = true) {
    if (stream_ == nullptr) {
      throw std::runtime_error("cannot write to null stream");
    }
    cl_stream_xfer_req req{0};
    if (eos) {
      req.flags = CL_STREAM_EOT;
    }
    req.priv_data = const_cast<char*>(name_.c_str());
    cl_int err;
    clWriteStream(stream_, const_cast<T*>(host_ptr), size * sizeof(T), &req,
                  &err);
    CL_CHECK(err);
  }
};

class Instance {
  cl::Device device_;
  cl::Context context_;
  cl::CommandQueue cmd_;
  cl::Program program_;
  cl::Kernel kernel_;
  std::unordered_map<int, cl::Memory> buffer_table_;
  std::unordered_map<int32_t, std::string> arg_table_;
  std::vector<cl::Memory> load_buffers_;
  std::vector<cl::Memory> store_buffers_;
  std::vector<cl::Event> load_event_;
  std::vector<cl::Event> compute_event_;
  std::vector<cl::Event> store_event_;

 public:
  Instance(const std::string& bitstream);

  // SetArg
  template <typename T>
  void SetArg(int index, T&& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
    kernel_.setArg(index, arg);
  }
  template <typename T>
  void SetArg(int index, RoBuf<T>&& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
    kernel_.setArg(index, buffer_table_[index]);
  }
  template <typename T>
  void SetArg(int index, WoBuf<T>&& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
    kernel_.setArg(index, buffer_table_[index]);
  }
  template <typename T>
  void SetArg(int index, RwBuf<T>&& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
    kernel_.setArg(index, buffer_table_[index]);
  }
  void SetArg(int index, WriteStream& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
  }
  void SetArg(int index, ReadStream& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
  }
  template <typename T, typename... Args>
  void SetArg(int index, T&& arg, Args&&... other_args) {
    SetArg(index, std::forward<T>(arg));
    SetArg(index + 1, std::forward<Args>(other_args)...);
  }
  template <typename... Args>
  void SetArg(Args&&... args) {
    SetArg(0, std::forward<Args>(args)...);
  }

  cl::Buffer CreateBuffer(int index, cl_mem_flags flags, size_t size,
                          void* host_ptr);

  // AllocateBuffers
  template <typename T>
  void AllocateBuffers(int index, T&& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
  }
  template <typename T>
  void AllocateBuffers(int index, WoBuf<T>&& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
    cl_mem_flags flags = CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY;
    cl::Buffer buffer = CreateBuffer(index, flags, arg.SizeInBytes(), arg);
    load_buffers_.push_back(buffer);
  }
  template <typename T>
  void AllocateBuffers(int index, RoBuf<T>&& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
    cl_mem_flags flags = CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY;
    cl::Buffer buffer = CreateBuffer(index, flags, arg.SizeInBytes(), arg);
    load_buffers_.push_back(buffer);
    store_buffers_.push_back(buffer);
  }
  template <typename T>
  void AllocateBuffers(int index, RwBuf<T>&& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
    cl_mem_flags flags = CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE;
    cl::Buffer buffer = CreateBuffer(index, flags, arg.SizeInBytes(), arg);
    store_buffers_.push_back(buffer);
  }
  void AllocateBuffers(int index, WriteStream& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
    arg.Attach(device_, kernel_, index);
  }
  void AllocateBuffers(int index, ReadStream& arg) {
#ifndef NDEBUG
    FUNC_INFO(index)
#endif
    arg.Attach(device_, kernel_, index);
  }

  template <typename T, typename... Args>
  void AllocateBuffers(int index, T&& arg, Args&&... other_args) {
    AllocateBuffers(index, std::forward<T>(arg));
    AllocateBuffers(index + 1, std::forward<Args>(other_args)...);
  }
  template <typename... Args>
  void AllocateBuffers(Args&&... args) {
    AllocateBuffers(0, std::forward<Args>(args)...);
  }

  void WriteToDevice();
  void ReadFromDevice();
  void Exec();
  void Finish();

  template <cl_profiling_info name>
  cl_ulong LoadProfilingInfo();
  template <cl_profiling_info name>
  cl_ulong ComputeProfilingInfo();
  template <cl_profiling_info name>
  cl_ulong StoreProfilingInfo();
  cl_ulong LoadTimeNanoSeconds();
  cl_ulong ComputeTimeNanoSeconds();
  cl_ulong StoreTimeNanoSeconds();
  double LoadTimeSeconds();
  double ComputeTimeSeconds();
  double StoreTimeSeconds();
  double LoadThroughputGbps();
  double StoreThroughputGbps();
};

template <typename... Args>
Instance Invoke(const std::string& bitstream, Args&&... args) {
  auto instance = Instance(bitstream);
  instance.AllocateBuffers(std::forward<Args>(args)...);
  instance.WriteToDevice();
  instance.SetArg(std::forward<Args>(args)...);
  instance.Exec();
  instance.ReadFromDevice();
  bool has_stream = false;
  bool dummy[sizeof...(Args)]{
      (has_stream |=
       std::is_base_of<Stream,
                       typename std::remove_reference<Args>::type>::value)...};
  if (!has_stream) {
#ifndef NDEBUG
    std::clog << "DEBUG: no stream found; waiting for command to finish"
              << std::endl;
#endif
    instance.Finish();
  }
  return instance;
}

#undef FUNC_INFO
#ifndef KEEP_CL_CHECK
#undef CL_CHECK
#endif  // KEEP_CL_CHECK

}  // namespace fpga

#endif  // FPGA_RUNTIME_H_
