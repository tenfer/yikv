#pragma once

#include <string>

namespace yikv {
namespace db {

// Advisory exclusive lock over a filesystem path ("touch" via O_CREAT, then flock).
// Held for the lifetime of the object; released on destruction (close(fd)).
class ArenaExclusiveLock {
public:
    explicit ArenaExclusiveLock(const std::string& lock_path);
    ~ArenaExclusiveLock();

    ArenaExclusiveLock(const ArenaExclusiveLock&)            = delete;
    ArenaExclusiveLock& operator=(const ArenaExclusiveLock&) = delete;

    ArenaExclusiveLock(ArenaExclusiveLock&& other) noexcept;
    ArenaExclusiveLock& operator=(ArenaExclusiveLock&& other) noexcept;

private:
    int fd_ = -1;
};

}  // namespace db
}  // namespace yikv
