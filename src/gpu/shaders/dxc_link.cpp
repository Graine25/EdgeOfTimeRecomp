#include "gpu/shaders/dxc_link.h"

#include <cstdio>
#include <string>

#include <unknwn.h>

#include <dxcapi.h>

namespace eot::gpu {
namespace {

template <typename T> class Com {
public:
  Com() = default;
  ~Com() {
    if (p_)
      p_->Release();
  }
  Com(const Com &) = delete;
  Com &operator=(const Com &) = delete;
  T **put() { return &p_; }
  T *operator->() const { return p_; }
  T *get() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

private:
  T *p_ = nullptr;
};

std::vector<uint8_t> ToVector(IDxcBlob *blob) {
  const auto *bytes = static_cast<const uint8_t *>(blob->GetBufferPointer());
  return std::vector<uint8_t>(bytes, bytes + blob->GetBufferSize());
}

}

std::vector<uint8_t> CompileSpecConstantLib(uint32_t value) {
  Com<IDxcCompiler3> compiler;
  if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put()))))
    return {};

  char hlsl[128];
  const int len = std::snprintf(hlsl, sizeof(hlsl),
                                "export uint g_SpecConstants() { return %u; }", value);
  DxcBuffer buffer{};
  buffer.Ptr = hlsl;
  buffer.Size = static_cast<SIZE_T>(len);
  buffer.Encoding = DXC_CP_ACP;

  const wchar_t *args[] = {L"-T", L"lib_6_3"};
  Com<IDxcResult> result;
  if (FAILED(compiler->Compile(&buffer, args, 2, nullptr, IID_PPV_ARGS(result.put()))) ||
      !result)
    return {};

  Com<IDxcBlob> blob;
  if (FAILED(result->GetResult(blob.put())) || !blob || blob->GetBufferSize() == 0)
    return {};
  return ToVector(blob.get());
}

std::vector<uint8_t> LinkSpecConstantLib(const uint8_t *library_dxil, uint32_t library_dxil_size,
                                         const uint8_t *spec_lib, size_t spec_lib_size,
                                         const wchar_t *profile, std::string *error_out) {
  if (error_out)
    error_out->clear();
  if (!library_dxil || library_dxil_size == 0 || !spec_lib || spec_lib_size == 0) {
    if (error_out)
      *error_out = "empty input blob";
    return {};
  }

  Com<IDxcUtils> utils;
  if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.put()))))
    return {};

  Com<IDxcBlobEncoding> spec_blob;
  Com<IDxcBlobEncoding> shader_blob;
  if (FAILED(utils->CreateBlobFromPinned(spec_lib, static_cast<UINT32>(spec_lib_size),
                                         DXC_CP_ACP, spec_blob.put())) ||
      FAILED(utils->CreateBlobFromPinned(library_dxil, library_dxil_size, DXC_CP_ACP,
                                         shader_blob.put())))
    return {};

  Com<IDxcLinker> linker;
  if (FAILED(DxcCreateInstance(CLSID_DxcLinker, IID_PPV_ARGS(linker.put()))))
    return {};

  linker->RegisterLibrary(L"SpecConstants", spec_blob.get());
  linker->RegisterLibrary(L"Shader", shader_blob.get());
  const wchar_t *lib_names[] = {L"SpecConstants", L"Shader"};

  Com<IDxcOperationResult> result;
  if (FAILED(linker->Link(L"shaderMain", profile, lib_names, 2, nullptr, 0, result.put())) ||
      !result) {
    if (error_out)
      *error_out = "IDxcLinker::Link call failed";
    return {};
  }
  HRESULT status = E_FAIL;
  if (FAILED(result->GetStatus(&status)) || FAILED(status)) {
    if (error_out) {
      Com<IDxcBlobEncoding> errors;
      if (SUCCEEDED(result->GetErrorBuffer(errors.put())) && errors &&
          errors->GetBufferSize() > 0) {
        error_out->assign(static_cast<const char *>(errors->GetBufferPointer()),
                          errors->GetBufferSize());
      } else {
        *error_out = "link status " + std::to_string(static_cast<long>(status));
      }
    }
    return {};
  }

  Com<IDxcBlob> linked;
  if (FAILED(result->GetResult(linked.put())) || !linked || linked->GetBufferSize() == 0)
    return {};
  return ToVector(linked.get());
}

}
