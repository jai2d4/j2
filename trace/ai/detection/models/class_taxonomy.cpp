#include "ai/detection/models/class_taxonomy.h"

#include <algorithm>
#include <unordered_set>

#include "core/common/string_utils.h"

namespace trace {
namespace {

/// Classes TRACE treats as vehicles. Deliberately conservative: these are the
/// observable transport classes of the COCO taxonomy, nothing inferred.
const std::unordered_set<std::string>& vehicleLabels() {
    static const std::unordered_set<std::string> kVehicles = {
        "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat"};
    return kVehicles;
}

}  // namespace

const std::vector<std::string>& cocoClassLabels() {
    static const std::vector<std::string> kLabels = {
        "person",        "bicycle",      "car",           "motorcycle",    "airplane",
        "bus",           "train",        "truck",         "boat",          "traffic light",
        "fire hydrant",  "stop sign",    "parking meter", "bench",         "bird",
        "cat",           "dog",          "horse",         "sheep",         "cow",
        "elephant",      "bear",         "zebra",         "giraffe",       "backpack",
        "umbrella",      "handbag",      "tie",           "suitcase",      "frisbee",
        "skis",          "snowboard",    "sports ball",   "kite",          "baseball bat",
        "baseball glove","skateboard",   "surfboard",     "tennis racket", "bottle",
        "wine glass",    "cup",          "fork",          "knife",         "spoon",
        "bowl",          "banana",       "apple",         "sandwich",      "orange",
        "broccoli",      "carrot",       "hot dog",       "pizza",         "donut",
        "cake",          "chair",        "couch",         "potted plant",  "bed",
        "dining table",  "toilet",       "tv",            "laptop",        "mouse",
        "remote",        "keyboard",     "cell phone",    "microwave",     "oven",
        "toaster",       "sink",         "refrigerator",  "book",          "clock",
        "vase",          "scissors",     "teddy bear",    "hair drier",    "toothbrush"};
    return kLabels;
}

DetectionClassGroup classGroupForLabel(const std::string& label) {
    const std::string normalised = toLower(trim(label));
    if (normalised == "person") return DetectionClassGroup::Person;
    if (vehicleLabels().count(normalised) > 0) return DetectionClassGroup::Vehicle;
    return DetectionClassGroup::Object;
}

std::string labelForClassId(const std::vector<std::string>& classes, int classId) {
    if (classId >= 0 && classId < static_cast<int>(classes.size())) {
        return classes[static_cast<std::size_t>(classId)];
    }
    return "class_" + std::to_string(classId);
}

}  // namespace trace
