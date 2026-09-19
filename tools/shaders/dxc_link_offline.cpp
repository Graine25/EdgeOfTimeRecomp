#include <windows.h>
#include <unknwn.h>
#include <dxcapi.h>
#include <cstdio>
#include <string>
#include <vector>
typedef HRESULT(__stdcall *PFN_Create)(REFCLSID, REFIID, LPVOID *);
static std::vector<uint8_t> ReadAll(const char *p) { FILE *f = fopen(p, "rb"); std::vector<uint8_t> v; if (!f) return v; fseek(f, 0, SEEK_END); v.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(v.data(), 1, v.size(), f); fclose(f); return v; }
int main(int argc, char **argv) {
  if (argc < 4) { printf("usage\n"); return 1; }
  HMODULE m = LoadLibraryA("dxcompiler.dll"); if (!m) { printf("no dxcompiler.dll\n"); return 1; }
  auto create = (PFN_Create)GetProcAddress(m, "DxcCreateInstance");
  IDxcCompiler3 *compiler = nullptr; create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
  IDxcUtils *utils = nullptr; create(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
  char hlsl[128]; int len = snprintf(hlsl, sizeof(hlsl), "export uint g_SpecConstants() { return %u; }", (unsigned)atoi(argv[2]));
  DxcBuffer buf{hlsl, (SIZE_T)len, DXC_CP_ACP};
  const wchar_t *args[] = {L"-T", L"lib_6_3"};
  IDxcResult *res = nullptr; compiler->Compile(&buf, args, 2, nullptr, IID_PPV_ARGS(&res));
  IDxcBlob *spec = nullptr; res->GetResult(&spec);
  auto lib = ReadAll(argv[1]);
  IDxcBlobEncoding *shader = nullptr; utils->CreateBlob(lib.data(), (UINT32)lib.size(), DXC_CP_ACP, &shader);
  IDxcLinker *linker = nullptr; create(CLSID_DxcLinker, IID_PPV_ARGS(&linker));
  linker->RegisterLibrary(L"SpecConstants", spec);
  linker->RegisterLibrary(L"Shader", shader);
  const wchar_t *libs[] = {L"SpecConstants", L"Shader"};
  IDxcOperationResult *lr = nullptr;
  HRESULT hr = linker->Link(L"shaderMain", L"ps_6_0", libs, 2, nullptr, 0, &lr);
  HRESULT status = E_FAIL; if (lr) lr->GetStatus(&status);
  printf("link hr=%08lx status=%08lx\n", (unsigned long)hr, (unsigned long)status);
  IDxcBlobEncoding *err = nullptr; if (lr) lr->GetErrorBuffer(&err);
  if (err && err->GetBufferSize()) printf("errors: %.*s\n", (int)err->GetBufferSize(), (const char *)err->GetBufferPointer());
  IDxcBlob *out = nullptr; if (lr) lr->GetResult(&out);
  if (!out || !out->GetBufferSize()) { printf("no output\n"); return 2; }
  FILE *f = fopen(argv[3], "wb"); fwrite(out->GetBufferPointer(), 1, out->GetBufferSize(), f); fclose(f);
  printf("wrote %zu bytes\n", (size_t)out->GetBufferSize());
  return 0;
}
