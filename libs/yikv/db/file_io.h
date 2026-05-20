#pragma once

#include <string>

namespace yikv {
namespace db {

std::string ReadWholeFile(const std::string& path);
void        AtomicWriteFile(const std::string& path, const std::string& data);

}  // namespace db
}  // namespace yikv
