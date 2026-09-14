#include "platform/language.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace eot::platform {

uint32_t SystemXLanguage() {
#if defined(_WIN32)
  switch (PRIMARYLANGID(::GetUserDefaultUILanguage())) {
  case LANG_FRENCH:
    return kXLanguageFrench;
  case LANG_ITALIAN:
    return kXLanguageItalian;
  case LANG_GERMAN:
    return kXLanguageGerman;
  case LANG_SPANISH:
    return kXLanguageSpanish;
  default:
    return kXLanguageEnglish;
  }
#else
  return kXLanguageEnglish;
#endif
}

uint32_t XLanguageFor(std::string_view eot_language) {
  if (eot_language == "fr")
    return kXLanguageFrench;
  if (eot_language == "it")
    return kXLanguageItalian;
  if (eot_language == "de")
    return kXLanguageGerman;
  if (eot_language == "es")
    return kXLanguageSpanish;
  if (eot_language == "en")
    return kXLanguageEnglish;
  return SystemXLanguage();
}

const char *XLanguageName(uint32_t xlanguage) {
  switch (xlanguage) {
  case kXLanguageFrench:
    return "French";
  case kXLanguageItalian:
    return "Italian";
  case kXLanguageGerman:
    return "German";
  case kXLanguageSpanish:
    return "Spanish";
  default:
    return "English";
  }
}

}
