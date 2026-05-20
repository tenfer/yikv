#include "container/string.h"

#include <algorithm>

namespace yikv {
namespace container {

String::String(Allocator* alloc, uint64_t root_off)
    : alloc_(alloc), base_(alloc->BaseAddress()), root_off_(root_off) {
    if (root_off_ == 0) {
        auto* r = static_cast<StrRoot*>(alloc_->Malloc(sizeof(StrRoot)));
        r->magic = kMagic;
        r->version = kVer;
        r->_pad = 0;
        r->size = 0;
        r->capacity = 1;
        char* mem = static_cast<char*>(alloc_->Malloc(1));
        mem[0] = '\0';
        r->data_off = off_of(mem);
        root_off_ = off_of(r);
        alloc_->PublishFence();
        return;
    }

    const auto* r = root();
    if (r->magic != kMagic) throw std::runtime_error("String: bad magic");
    if (r->version != kVer) throw std::runtime_error("String: bad version");
}

size_t String::size() const noexcept { return root()->size; }
size_t String::capacity() const noexcept { return root()->capacity; }

const char* String::data() const noexcept { return at_off(root()->data_off); }

std::string_view String::view() const noexcept {
    return std::string_view(data(), size());
}

std::string String::str() const { return std::string(view()); }

void String::clear() {
    auto* r = root();
    r->size = 0;
    at_off(r->data_off)[0] = '\0';
    alloc_->PublishFence();
}

void String::assign(std::string_view s) {
    ensure_capacity(s.size() + 1);
    auto* r = root();
    if (!s.empty()) std::memcpy(at_off(r->data_off), s.data(), s.size());
    at_off(r->data_off)[s.size()] = '\0';
    r->size = s.size();
    alloc_->PublishFence();
}

void String::append(std::string_view s) {
    if (s.empty()) return;
    auto* r = root();
    size_t new_sz = r->size + s.size();
    ensure_capacity(new_sz + 1);
    r = root();
    std::memcpy(at_off(r->data_off) + r->size, s.data(), s.size());
    at_off(r->data_off)[new_sz] = '\0';
    r->size = new_sz;
    alloc_->PublishFence();
}

void String::push_back(char c) {
    auto* r = root();
    ensure_capacity(r->size + 2);
    r = root();
    at_off(r->data_off)[r->size] = c;
    ++r->size;
    at_off(r->data_off)[r->size] = '\0';
    alloc_->PublishFence();
}

int String::compare(std::string_view s) const noexcept {
    const auto v = view();
    const size_t n = std::min(v.size(), s.size());
    int rc = std::memcmp(v.data(), s.data(), n);
    if (rc != 0) return rc;
    if (v.size() < s.size()) return -1;
    if (v.size() > s.size()) return 1;
    return 0;
}

std::string String::substr(size_t pos, size_t count) const {
    auto v = view();
    if (pos > v.size()) throw std::out_of_range("String::substr");
    return std::string(v.substr(pos, count));
}

size_t String::find(std::string_view needle, size_t pos) const noexcept {
    return view().find(needle, pos);
}

bool String::starts_with(std::string_view prefix) const noexcept {
    auto v = view();
    return v.size() >= prefix.size() && v.compare(0, prefix.size(), prefix) == 0;
}

bool String::ends_with(std::string_view suffix) const noexcept {
    auto v = view();
    if (suffix.size() > v.size()) return false;
    return v.compare(v.size() - suffix.size(), suffix.size(), suffix) == 0;
}

String::StrRoot* String::root() noexcept {
    return reinterpret_cast<StrRoot*>(static_cast<char*>(base_) + root_off_);
}
const String::StrRoot* String::root() const noexcept {
    return reinterpret_cast<const StrRoot*>(static_cast<const char*>(base_) + root_off_);
}
char* String::at_off(uint64_t off) noexcept {
    return static_cast<char*>(base_) + off;
}
const char* String::at_off(uint64_t off) const noexcept {
    return static_cast<const char*>(base_) + off;
}
uint64_t String::off_of(const void* p) const noexcept {
    return static_cast<uint64_t>(static_cast<const char*>(p) - static_cast<const char*>(base_));
}

void String::ensure_capacity(size_t need) {
    auto* r = root();
    if (r->capacity >= need) return;
    size_t new_cap = std::max<size_t>(need, r->capacity * 2);
    char* mem = static_cast<char*>(alloc_->Malloc(new_cap));
    std::memcpy(mem, at_off(r->data_off), r->size + 1);
    alloc_->Free(at_off(r->data_off), FreeMode::Delayed);
    r->data_off = off_of(mem);
    r->capacity = new_cap;
}

}  // namespace container
}  // namespace yikv
