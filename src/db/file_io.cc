#include "src/db/file_io.h"

#include <cerrno>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <unistd.h>

namespace yikv {
namespace db {

std::string ReadWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::system_error(errno, std::generic_category(),
                                "ReadWholeFile: open " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void AtomicWriteFile(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::system_error(errno, std::generic_category(),
                                    "AtomicWriteFile: open " + tmp);
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            throw std::runtime_error("AtomicWriteFile: write " + tmp);
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "AtomicWriteFile: rename to " + path);
    }
}

}  // namespace db
}  // namespace yikv
