#pragma once

#include "src/alloc/allocator.h"
#include "src/schema/schema.h"

namespace yikv {
namespace index {

// Abstract base for arena-backed indexes (KVIndex, InvertedIndex, …).
// Owns allocator/schema bindings and compiled layout; concrete types add maps
// and implement Publish().
class Index {
public:
    virtual ~Index() = default;

    virtual void Publish() = 0;

    alloc::Allocator*       alloc()   const noexcept { return alloc_; }
    const schema::Schema*   schema()  const noexcept { return schema_; }
    const schema::CompiledSchema& compiled() const noexcept { return compiled_; }

protected:
    explicit Index(alloc::Allocator* alloc, const schema::Schema* schema);

    alloc::Allocator*      alloc_;
    const schema::Schema*  schema_;
    schema::CompiledSchema compiled_;
};

}  // namespace index
}  // namespace yikv
