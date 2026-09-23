#pragma once

#include <stdint.h>

#if defined(EOT_MODS_HOST) && defined(_WIN32)
#define EOT_MODS_API __declspec(dllexport)
#elif defined(EOT_MODS_HOST)
#define EOT_MODS_API __attribute__((visibility("default")))
#else
#define EOT_MODS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum eot_mod_slot_t { EOT_MOD_SLOT_PAK = 0, EOT_MOD_SLOT_MODEL = 1, EOT_MOD_SLOT_BINARY = 2, EOT_MOD_SLOT_COUNT = 3 };

struct eot_mod_slot_info {
  int32_t installed;
  char files[384];
  char note[256];
};

enum eot_mods_import_state_t {
  EOT_MODS_IDLE = 0,
  EOT_MODS_CHOOSING = 1,
  EOT_MODS_IMPORTING = 2,
  EOT_MODS_DONE = 3,
  EOT_MODS_FAILED = 4
};

EOT_MODS_API int32_t eot_mods_slot(int32_t slot, struct eot_mod_slot_info *out);
EOT_MODS_API int32_t eot_mods_import_begin(int32_t slot);
EOT_MODS_API int32_t eot_mods_import_state(char *message, int32_t size);
EOT_MODS_API void eot_mods_import_acknowledge(void);
EOT_MODS_API int32_t eot_mods_restore(int32_t slot, char *message, int32_t size);
EOT_MODS_API int32_t eot_mods_open_folder(void);
EOT_MODS_API int32_t eot_mods_languages(char *out, int32_t size);

#ifdef __cplusplus
}
#endif
