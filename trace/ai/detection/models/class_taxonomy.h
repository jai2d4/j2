#pragma once

#include <string>
#include <vector>

#include "core/models/detection.h"

namespace trace {

/// The 80 COCO classes, in the canonical order used by the detection models
/// TRACE ships with. A model's class list is a property of that model, not of
/// TRACE: this is the default list, and a model descriptor may override it.
const std::vector<std::string>& cocoClassLabels();

/// Maps a class label onto the coarse group used by the timeline lanes and the
/// filters. Unknown labels group as Object rather than being dropped: a model
/// TRACE does not recognise still produces usable observations.
DetectionClassGroup classGroupForLabel(const std::string& label);

/// Human-readable label for a class index in the given list, or a stable
/// "class_<n>" when the index falls outside it — never an invented name.
std::string labelForClassId(const std::vector<std::string>& classes, int classId);

}  // namespace trace
