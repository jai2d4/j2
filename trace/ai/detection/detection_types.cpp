#include "ai/detection/detection_types.h"

namespace trace {

const char* toString(DevicePreference preference) {
    switch (preference) {
        case DevicePreference::Auto: return "auto";
        case DevicePreference::Cpu:  return "cpu";
        case DevicePreference::Gpu:  return "gpu";
    }
    return "auto";
}

const char* toDisplayString(DevicePreference preference) {
    switch (preference) {
        case DevicePreference::Auto: return "Auto";
        case DevicePreference::Cpu:  return "CPU";
        case DevicePreference::Gpu:  return "GPU";
    }
    return "Auto";
}

DevicePreference devicePreferenceFromString(const std::string& text, DevicePreference fallback) {
    if (text == "auto") return DevicePreference::Auto;
    if (text == "cpu") return DevicePreference::Cpu;
    if (text == "gpu") return DevicePreference::Gpu;
    return fallback;
}

}  // namespace trace
