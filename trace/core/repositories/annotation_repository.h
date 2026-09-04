#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/annotation.h"

namespace trace {

class AnnotationRepository {
public:
    explicit AnnotationRepository(std::shared_ptr<Database> database);

    Status insert(const Annotation& value);
    Status update(const Annotation& value);
    Status remove(const std::string& annotationId);

    Result<std::optional<Annotation>> findById(const std::string& annotationId);
    Result<std::vector<Annotation>> listForEvidence(const std::string& evidenceId);
    Result<std::vector<Annotation>> listForCase(const std::string& caseId);
    Result<std::int64_t> countForEvidence(const std::string& evidenceId);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
