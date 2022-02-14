#include "frt/xilinx_opencl_device.h"

#include <memory>
#include <stdexcept>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <tinyxml.h>
#include <xclbin.h>

#include "frt/opencl_util.h"
#include "frt/stream_wrapper.h"
#include "frt/tag.h"
#include "frt/xilinx_opencl_stream.h"

namespace fpga {
namespace internal {

namespace {

std::string Exec(const std::string& cmd) {
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                pclose);
  if (pipe == nullptr) {
    throw std::runtime_error(std::string{"cannot execute: "} + cmd);
  }
  int c;
  while (c = fgetc(pipe.get()), !feof(pipe.get())) {
    result += static_cast<char>(c);
  }
  return result;
}

void UpdateEnviron(const std::string& script) {
  std::string cmd = "source " + script + " >/dev/null 2>&1 && env -0";
  cmd = "bash -c '" + cmd + "'";
  const std::string envs = internal::Exec(cmd);
  for (size_t n = 0; n < envs.size();) {
    const std::string env = envs.c_str() + n;
    n += env.size() + 1;

    const auto pos = env.find('=');
    const auto name = env.substr(0, pos);
    const auto new_value = env.substr(pos + 1);
    const auto old_value = getenv(name.c_str());
    if (old_value == nullptr || new_value != old_value) {
      setenv(name.c_str(), new_value.c_str(), /*replace=*/1);
    }
  }
}

}  // namespace

XilinxOpenclDevice::XilinxOpenclDevice(const cl::Program::Binaries& binaries) {
  std::string target_device_name;
  std::vector<std::string> kernel_names;
  std::vector<int> kernel_arg_counts;
  int arg_count = 0;
  const auto axlf_top = reinterpret_cast<const axlf*>(binaries.begin()->data());
  switch (axlf_top->m_header.m_mode) {
    case XCLBIN_FLAT:
    case XCLBIN_PR:
    case XCLBIN_TANDEM_STAGE2:
    case XCLBIN_TANDEM_STAGE2_WITH_PR:
      break;
    case XCLBIN_HW_EMU:
      setenv("XCL_EMULATION_MODE", "hw_emu", 0);
      break;
    case XCLBIN_SW_EMU:
      setenv("XCL_EMULATION_MODE", "sw_emu", 0);
      break;
    default:
      throw std::runtime_error("unknown xclbin mode");
  }
  target_device_name =
      reinterpret_cast<const char*>(axlf_top->m_header.m_platformVBNV);
  if (auto metadata = xclbin::get_axlf_section(axlf_top, EMBEDDED_METADATA)) {
    TiXmlDocument doc;
    doc.Parse(
        reinterpret_cast<const char*>(axlf_top) + metadata->m_sectionOffset,
        nullptr, TIXML_ENCODING_UTF8);
    auto xml_core = doc.FirstChildElement("project")
                        ->FirstChildElement("platform")
                        ->FirstChildElement("device")
                        ->FirstChildElement("core");
    std::string target_meta = xml_core->Attribute("target");
    for (auto xml_kernel = xml_core->FirstChildElement("kernel");
         xml_kernel != nullptr;
         xml_kernel = xml_kernel->NextSiblingElement("kernel")) {
      kernel_names.push_back(xml_kernel->Attribute("name"));
      kernel_arg_counts.push_back(arg_count);
      for (auto xml_arg = xml_kernel->FirstChildElement("arg");
           xml_arg != nullptr; xml_arg = xml_arg->NextSiblingElement("arg")) {
        auto& arg = arg_table_[arg_count];
        arg.index = arg_count;
        ++arg_count;
        arg.name = xml_arg->Attribute("name");
        arg.type = xml_arg->Attribute("type");
        auto cat = atoi(xml_arg->Attribute("addressQualifier"));
        switch (cat) {
          case 0:
            arg.cat = ArgInfo::kScalar;
            break;
          case 1:
            arg.cat = ArgInfo::kMmap;
            break;
          case 4:
            arg.cat = ArgInfo::kStream;
            break;
          default:
            std::clog << "WARNING: Unknown argument category: " << cat;
        }
      }
    }
    // m_mode doesn't always work
    if (target_meta == "hw_em") {
      setenv("XCL_EMULATION_MODE", "hw_emu", 0);
    } else if (target_meta == "csim") {
      setenv("XCL_EMULATION_MODE", "sw_emu", 0);
    }
  } else {
    throw std::runtime_error("cannot determine kernel name from binary");
  }

  if (const char* xcl_emulation_mode = getenv("XCL_EMULATION_MODE")) {
    std::string xilinx_tool;
    for (const auto env : {
             "XILINX_VITIS",
             "XILINX_SDX",
             "XILINX_HLS",
             "XILINX_VIVADO",
         }) {
      if (const auto value = getenv(env)) {
        xilinx_tool = value;
        break;
      }
    }

    if (xilinx_tool.empty()) {
      for (const std::string hls : {"vitis_hls", "vivado_hls"}) {
        std::string cmd =
            "bash -c '" + hls + " -version -help -l /dev/null 2>&-'";
        std::istringstream lines(internal::Exec(cmd));
        for (std::string line; getline(lines, line);) {
          const std::string prefix = "source ";
          const std::string suffix = "/scripts/" + hls + "/hls.tcl -notrace";
          if (line.size() > prefix.size() + suffix.size() &&
              line.compare(0, prefix.size(), prefix) == 0 &&
              line.compare(line.size() - suffix.size(), suffix.size(),
                           suffix) == 0) {
            xilinx_tool = line.substr(
                prefix.size(), line.size() - prefix.size() - suffix.size());
            break;
          }
        }
      }
    }

    internal::UpdateEnviron(xilinx_tool + "/settings64.sh");
    if (const auto xrt = getenv("XILINX_XRT")) {
      internal::UpdateEnviron(std::string(xrt) + "/setup.sh");
    }

    const auto uid = std::to_string(geteuid());

    // Vitis software simulation stucks without $USER.
    setenv("USER", uid.c_str(), /* __replace = */ 0);

    const char* tmpdir_or_null = getenv("TMPDIR");
    std::string tmpdir = tmpdir_or_null ? tmpdir_or_null : "/tmp";
    tmpdir += "/.frt." + uid;
    if (mkdir(tmpdir.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) && errno != EEXIST) {
      throw std::runtime_error(std::string("cannot create tmpdir ") + tmpdir +
                               ": " + strerror(errno));
    }

    // If SDACCEL_EM_RUN_DIR is not set, use a per-use tmpdir for `.run`.
    setenv("SDACCEL_EM_RUN_DIR", tmpdir.c_str(), 0);

    // If EMCONFIG_PATH is not set, use a per-user and per-device tmpdir to
    // cache `emconfig.json`.

    std::string emconfig_dir;
    if (const char* emconfig_dir_or_null = getenv("EMCONFIG_PATH")) {
      emconfig_dir = emconfig_dir_or_null;
    } else {
      emconfig_dir = tmpdir;
      emconfig_dir += "/emconfig.";
      emconfig_dir += target_device_name;
      setenv("EMCONFIG_PATH", emconfig_dir.c_str(), 0);
    }

    // Generate `emconfig.json` when necessary.
    std::string cmd =
        "jq --exit-status "
        "'.Platform.Boards[]|select(.Devices[]|select(.Name==\"";
    cmd += target_device_name;
    cmd += "\"))' ";
    cmd += emconfig_dir;
    cmd += "/emconfig.json >/dev/null 2>&1 || emconfigutil --platform ";
    cmd += target_device_name;
    cmd += " --od ";
    cmd += emconfig_dir;
    if (system(cmd.c_str())) {
      throw std::runtime_error("emconfigutil failed");
    }
  }

  Initialize(binaries, "Xilinx", target_device_name, kernel_names,
             kernel_arg_counts);
}

std::unique_ptr<Device> XilinxOpenclDevice::New(
    const cl::Program::Binaries& binaries) {
  if (binaries.size() != 1 || binaries.begin()->size() < 8 ||
      memcmp(binaries.begin()->data(), "xclbin2", 8) != 0) {
    return nullptr;
  }
  return std::make_unique<XilinxOpenclDevice>(binaries);
}

void XilinxOpenclDevice::SetStreamArg(int index, Tag tag, StreamWrapper& arg) {
  auto pair = GetKernel(index);
  arg.Attach(std::make_unique<XilinxOpenclStream>(
      arg.name, device_, pair.second, pair.first, tag));
}

void XilinxOpenclDevice::WriteToDevice() {
  if (!load_indices_.empty()) {
    load_event_.resize(1);
    CL_CHECK(cmd_.enqueueMigrateMemObjects(GetLoadBuffers(), /* flags = */ 0,
                                           /* events = */ nullptr,
                                           load_event_.data()));
  } else {
    load_event_.clear();
  }
}

void XilinxOpenclDevice::ReadFromDevice() {
  if (!store_indices_.empty()) {
    store_event_.resize(1);
    CL_CHECK(cmd_.enqueueMigrateMemObjects(
        GetStoreBuffers(), CL_MIGRATE_MEM_OBJECT_HOST, &compute_event_,
        store_event_.data()));
  } else {
    store_event_.clear();
  }
}

cl::Buffer XilinxOpenclDevice::CreateBuffer(int index, cl_mem_flags flags,
                                            void* host_ptr, size_t size) {
  flags |= CL_MEM_USE_HOST_PTR;
  return OpenclDevice::CreateBuffer(index, flags, host_ptr, size);
}

}  // namespace internal
}  // namespace fpga