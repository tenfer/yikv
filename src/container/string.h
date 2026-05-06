#pragma once

#include "src/alloc/allocator.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <stdexcept>

namespace yikv {
namespace container {

class String {
public:
    using Allocator   = yikv::alloc::Allocator;
    using FreeMode    = yikv::alloc::FreeMode;

    explicit String(Allocator* alloc, uint64_t root_off = 0);
    ~String() = default;

    String(const String&) = delete;
    String& operator=(const String&) = delete;
    String(String&&) noexcept = default;
    String& operator=(String&&) noexcept = default;

    uint64_t root_offset() const noexcept { return root_off_; }

    size_t size() const noexcept;
    size_t capacity() const noexcept;
    bool empty() const noexcept { return size() == 0; }

    const char* data() const noexcept;
    const char* c_str() const noexcept { return data(); }
    std::string_view view() const noexcept;
    std::string str() const;
    char operator[](size_t i) const noexcept { return data()[i]; }
    char at(size_t i) const {
        if (i >= size()) throw std::out_of_range("String::at");
        return data()[i];
    }

    void clear();
    void assign(std::string_view s);
    void append(std::string_view s);
    void push_back(char c);

    String& operator+=(std::string_view s) { append(s); return *this; }
    String& operator+=(char c) { push_back(c); return *this; }

    int compare(std::string_view s) const noexcept;
    bool operator==(std::string_view s) const noexcept { return compare(s) == 0; }
    bool operator!=(std::string_view s) const noexcept { return !(*this == s); }
    std::string substr(size_t pos = 0, size_t count = std::string::npos) const;
    size_t find(std::string_view needle, size_t pos = 0) const noexcept;
    bool starts_with(std::string_view prefix) const noexcept;
    bool ends_with(std::string_view suffix) const noexcept;

private:
    struct alignas(8) StrRoot {
        uint32_t magic;
        uint16_t version;
        uint16_t _pad;
        uint64_t size;
        uint64_t capacity;
        uint64_t data_off;
    };
    static_assert(sizeof(StrRoot) == 32);

    static constexpr uint32_t kMagic = 0x31475253u;  // "SRG1"
    static constexpr uint16_t kVer   = 1;

    Allocator*   alloc_    = nullptr;
    void*        base_     = nullptr;
    uint64_t     root_off_ = 0;

    StrRoot* root() noexcept;
    const StrRoot* root() const noexcept;
    char* at_off(uint64_t off) noexcept;
    const char* at_off(uint64_t off) const noexcept;
    uint64_t off_of(const void* p) const noexcept;

    void ensure_capacity(size_t need);
};

}  // namespace container
}  // namespace yikv
