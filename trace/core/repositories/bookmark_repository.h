#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/bookmark.h"

namespace trace {

class BookmarkRepository {
public:
    explicit BookmarkRepository(std::shared_ptr<Database> database);

    Status insert(const Bookmark& value);
    Status update(const Bookmark& value);
    Status remove(const std::string& bookmarkId);

    Result<std::optional<Bookmark>> findById(const std::string& bookmarkId);
    /// Ordered by media position, which is how the timeline draws them.
    Result<std::vector<Bookmark>> listForEvidence(const std::string& evidenceId);
    Result<std::vector<Bookmark>> listForCase(const std::string& caseId);
    Result<std::int64_t> countForEvidence(const std::string& evidenceId);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
