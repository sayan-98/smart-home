#include "core/Types.h"

#include <string.h>

namespace sh {

const char* toString(Source s) {
  switch (s) {
    case Source::Physical:   return "physical";
    case Source::App:        return "app";
    case Source::Alexa:      return "alexa";
    case Source::Schedule:   return "schedule";
    case Source::Automation: return "automation";
    case Source::Cloud:      return "cloud";
    case Source::Restore:    return "restore";
    case Source::Timer:      return "timer";
    case Source::Factory:    return "factory";
    default:                 return "unknown";
  }
}

Source sourceFromString(const char* s) {
  if (!s) return Source::Unknown;
  struct Entry { const char* name; Source value; };
  static const Entry kTable[] = {
      {"physical", Source::Physical},   {"app", Source::App},
      {"alexa", Source::Alexa},         {"schedule", Source::Schedule},
      {"automation", Source::Automation}, {"cloud", Source::Cloud},
      {"restore", Source::Restore},     {"timer", Source::Timer},
      {"factory", Source::Factory},
  };
  for (const auto& e : kTable) {
    if (strcmp(s, e.name) == 0) return e.value;
  }
  return Source::Unknown;
}

}  // namespace sh
