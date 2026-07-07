#include "gpu/shaders/dxc_link.h"

#include <unknwn.h>

#include <dxcapi.h>

#include <cstdio>
#include <cstring>

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

void CaptureErrors(IDxcOperationResult *result, std::string *error) {
  if (!error || !result)
    return;
  IDxcBlobEncoding *blob = nullptr;
  if (FAILED(result->GetErrorBuffer(&blob)) || !blob)
    return;
  const auto *text = static_cast<const char *>(blob->GetBufferPointer());
  if (text && blob->GetBufferSize() > 0)
    error->assign(text, strnlen(text, blob->GetBufferSize()));
  blob->Release();
}

std::vector<uint8_t> ToVector(IDxcBlob *blob) {
  const auto *bytes = static_cast<const uint8_t *>(blob->GetBufferPointer());
  return std::vector<uint8_t>(bytes, bytes + blob->GetBufferSize());
}

}

std::vector<uint8_t> CompileSpecConstantLib(uint32_t value,
                                           std::string *error) {
  Com<IDxcCompiler3> compiler;
  if (FAILED(
          DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())))) {
    return {};
  }

  char hlsl[128];
  const int len =
      std::snprintf(hlsl, sizeof(hlsl),
                    "export uint g_SpecConstants() { return %u; }", value);
  DxcBuffer buffer{};
  buffer.Ptr = hlsl;
  buffer.Size = static_cast<SIZE_T>(len);
  buffer.Encoding = DXC_CP_ACP;

  const wchar_t *args[] = {L"-T", L"lib_6_3"};
  Com<IDxcResult> result;
  if (FAILED(compiler->Compile(&buffer, args, 2, nullptr,
                               IID_PPV_ARGS(result.put()))) ||
      !result) {
    return {};
  }
  CaptureErrors(result.get(), error);

  Com<IDxcBlob> blob;
  if (FAILED(result->GetResult(blob.put())) || !blob ||
      blob->GetBufferSize() == 0) {
    return {};
  }
  return ToVector(blob.get());
}

std::vector<uint8_t> LinkSpecConstantLib(const uint8_t *libraryDxil,
                                         uint32_t libraryDxilSize,
                                         const uint8_t *specLib,
                                         size_t specLibSize,
                                         const wchar_t *profile,
                                         const wchar_t *entry,
                                         std::string *error) {
  if (!libraryDxil || libraryDxilSize == 0 || !specLib || specLibSize == 0)
    return {};

  Com<IDxcUtils> utils;
  if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.put()))))
    return {};

  Com<IDxcBlobEncoding> specBlob;
  Com<IDxcBlobEncoding> shaderBlob;
  if (FAILED(utils->CreateBlobFromPinned(specLib,
                                         static_cast<UINT32>(specLibSize),
                                         DXC_CP_ACP, specBlob.put())) ||
      FAILED(utils->CreateBlobFromPinned(libraryDxil, libraryDxilSize,
                                         DXC_CP_ACP, shaderBlob.put()))) {
    return {};
  }

  Com<IDxcLinker> linker;
  if (FAILED(DxcCreateInstance(CLSID_DxcLinker, IID_PPV_ARGS(linker.put()))))
    return {};

  linker->RegisterLibrary(L"SpecConstants", specBlob.get());
  linker->RegisterLibrary(L"Shader", shaderBlob.get());
  const wchar_t *libNames[] = {L"SpecConstants", L"Shader"};

  Com<IDxcOperationResult> result;
  if (FAILED(linker->Link(entry, profile, libNames, 2, nullptr, 0,
                          result.put())) ||
      !result) {
    return {};
  }
  CaptureErrors(result.get(), error);

  HRESULT status = E_FAIL;
  if (FAILED(result->GetStatus(&status)) || FAILED(status))
    return {};

  Com<IDxcBlob> linked;
  if (FAILED(result->GetResult(linked.put())) || !linked ||
      linked->GetBufferSize() == 0) {
    return {};
  }
  return ToVector(linked.get());
}

}
