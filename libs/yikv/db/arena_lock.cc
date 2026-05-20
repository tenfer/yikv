#include "db/arena_lock.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace yikv {
namespace db {

ArenaExclusiveLock::ArenaExclusiveLock(const std::string& lock_path) {
    fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "ArenaExclusiveLock: open " + lock_path);
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        ::close(fd_);
        fd_ = -1;
        std::string msg = "ArenaExclusiveLock: flock failed on " + lock_path + ": " +
                          std::strerror(e);
        if (e == EWOULDBLOCK || e == EAGAIN) {
            msg += " (another process may already have this index open)";
        }
        throw std::runtime_error(msg);
    }
}

ArenaExclusiveLock::~ArenaExclusiveLock() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

ArenaExclusiveLock::ArenaExclusiveLock(ArenaExclusiveLock&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

ArenaExclusiveLock& ArenaExclusiveLock::operator=(ArenaExclusiveLock&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_       = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

}  // namespace db
}  // namespace yikv
