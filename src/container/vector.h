#pragma once

#include "src/alloc/allocator.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace yikv {
namespace container {

template <class T>
class Vector {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Vector currently supports trivially-copyable T only");

public:
    using Allocator   = yikv::alloc::Allocator;
    using FreeMode    = yikv::alloc::FreeMode;

    explicit Vector(Allocator* alloc, uint64_t root_off = 0)
        : alloc_(alloc), base_(alloc->BaseAddress()), root_off_(root_off) {
        if (root_off_ == 0) {
            auto* r = static_cast<Root*>(alloc_->Malloc(sizeof(Root)));
            r->magic = kMagic;
            r->version = kVer;
            r->_pad = 0;
            r->size = 0;
            r->capacity = 4;
            r->data_off = off_of(alloc_->Malloc(r->capacity * sizeof(T)));
            root_off_ = off_of(r);
            alloc_->PublishFence();
            return;
        }
        const auto* r = root();
        if (r->magic != kMagic) throw std::runtime_error("Vector: bad magic");
        if (r->version != kVer) throw std::runtime_error("Vector: bad version");
    }

    uint64_t root_offset() const noexcept { return root_off_; }
    size_t size() const noexcept { return root()->size; }
    size_t capacity() const noexcept { return root()->capacity; }
    bool empty() const noexcept { return size() == 0; }

    using iterator = T*;
    using const_iterator = const T*;

    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + size(); }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + size(); }
    const_iterator cbegin() const noexcept { return data(); }
    const_iterator cend() const noexcept { return data() + size(); }

    const T& operator[](size_t i) const noexcept { return data()[i]; }
    T& operator[](size_t i) noexcept { return data()[i]; }
    const T& at(size_t i) const {
        if (i >= size()) throw std::out_of_range("Vector::at");
        return data()[i];
    }
    T& at(size_t i) {
        if (i >= size()) throw std::out_of_range("Vector::at");
        return data()[i];
    }
    const T& front() const {
        if (empty()) throw std::out_of_range("Vector::front");
        return data()[0];
    }
    T& front() {
        if (empty()) throw std::out_of_range("Vector::front");
        return data()[0];
    }
    const T& back() const {
        if (empty()) throw std::out_of_range("Vector::back");
        return data()[size() - 1];
    }
    T& back() {
        if (empty()) throw std::out_of_range("Vector::back");
        return data()[size() - 1];
    }

    const T* data() const noexcept { return at_off(root()->data_off); }
    T* data() noexcept { return at_off(root()->data_off); }

    void clear() {
        root()->size = 0;
        alloc_->PublishFence();
    }
    void reserve(size_t new_cap) {
        if (new_cap <= capacity()) return;
        reallocate(new_cap);
        alloc_->PublishFence();
    }
    void resize(size_t n, const T& value = T{}) {
        auto* r = root();
        if (n > r->capacity) reallocate(n);
        r = root();
        if (n > r->size) {
            for (size_t i = r->size; i < n; ++i) data()[i] = value;
        }
        r->size = n;
        alloc_->PublishFence();
    }

    void push_back(const T& v) {
        auto* r = root();
        if (r->size == r->capacity) grow();
        r = root();
        data()[r->size++] = v;
        alloc_->PublishFence();
    }
    template <class... Args>
    T& emplace_back(Args&&... args) {
        static_assert(sizeof...(Args) == 1,
                      "Vector::emplace_back currently supports one argument");
        auto* r = root();
        if (r->size == r->capacity) grow();
        r = root();
        T tmp(std::forward<Args>(args)...);
        data()[r->size++] = tmp;
        alloc_->PublishFence();
        return data()[r->size - 1];
    }

    void pop_back() {
        auto* r = root();
        if (r->size == 0) throw std::out_of_range("Vector::pop_back");
        --r->size;
        alloc_->PublishFence();
    }
    iterator insert(const_iterator pos, const T& value) {
        size_t idx = static_cast<size_t>(pos - cbegin());
        if (idx > size()) throw std::out_of_range("Vector::insert");
        auto* r = root();
        if (r->size == r->capacity) grow();
        r = root();
        T* d = data();
        std::memmove(d + idx + 1, d + idx, (r->size - idx) * sizeof(T));
        d[idx] = value;
        ++r->size;
        alloc_->PublishFence();
        return begin() + idx;
    }
    iterator erase(const_iterator pos) {
        size_t idx = static_cast<size_t>(pos - cbegin());
        auto* r = root();
        if (idx >= r->size) throw std::out_of_range("Vector::erase");
        T* d = data();
        std::memmove(d + idx, d + idx + 1, (r->size - idx - 1) * sizeof(T));
        --r->size;
        alloc_->PublishFence();
        return begin() + idx;
    }

private:
    struct alignas(8) Root {
        uint32_t magic;
        uint16_t version;
        uint16_t _pad;
        uint64_t size;
        uint64_t capacity;
        uint64_t data_off;
    };
    static_assert(sizeof(Root) == 32);

    static constexpr uint32_t kMagic = 0x31544356u;  // "VCT1"
    static constexpr uint16_t kVer   = 1;

    Allocator*   alloc_    = nullptr;
    void*        base_     = nullptr;
    uint64_t     root_off_ = 0;

    Root* root() noexcept {
        return reinterpret_cast<Root*>(static_cast<char*>(base_) + root_off_);
    }
    const Root* root() const noexcept {
        return reinterpret_cast<const Root*>(static_cast<const char*>(base_) + root_off_);
    }
    T* at_off(uint64_t off) noexcept {
        return reinterpret_cast<T*>(static_cast<char*>(base_) + off);
    }
    const T* at_off(uint64_t off) const noexcept {
        return reinterpret_cast<const T*>(static_cast<const char*>(base_) + off);
    }
    uint64_t off_of(const void* p) const noexcept {
        return static_cast<uint64_t>(
            static_cast<const char*>(p) - static_cast<const char*>(base_));
    }

    void grow() {
        reallocate(root()->capacity * 2);
    }
    void reallocate(size_t new_cap) {
        auto* r = root();
        T* mem = static_cast<T*>(alloc_->Malloc(new_cap * sizeof(T)));
        std::memcpy(mem, data(), r->size * sizeof(T));
        alloc_->Free(data(), FreeMode::Delayed);
        r->data_off = off_of(mem);
        r->capacity = new_cap;
    }
};

}  // namespace container
}  // namespace yikv
