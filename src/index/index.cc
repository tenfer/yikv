#include "src/index/index.h"

namespace yikv {
namespace index {

Index::Index(alloc::Allocator* alloc, const schema::Schema* schema)
    : alloc_(alloc), schema_(schema) {
    std::string err;
    schema_->Compile(&compiled_, &err);
}

}  // namespace index
}  // namespace yikv
