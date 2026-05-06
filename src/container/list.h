#pragma once

#include "src/alloc/allocator.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <type_traits>

namespace yikv {
namespace container {

template <class T>
class List {
    static_assert(std::is_trivially_copyable_v<T>,
                  "List currently supports trivially-copyable T only");

public:
    using Allocator   = yikv::alloc::Allocator;
    using FreeMode    = yikv::alloc::FreeMode;

    explicit List(Allocator* alloc, uint64_t root_off = 0)
        : alloc_(alloc), base_(alloc->BaseAddress()), root_off_(root_off) {
        if (root_off_ == 0) {
            auto* r = static_cast<Root*>(alloc_->Malloc(sizeof(Root)));
            r->magic = kMagic;
            r->version = kVer;
            r->_pad = 0;
            r->size = 0;
            r->head_off = 0;
            r->tail_off = 0;
            root_off_ = off_of(r);
            alloc_->PublishFence();
            return;
        }
        const auto* r = root();
        if (r->magic != kMagic) throw std::runtime_error("List: bad magic");
        if (r->version != kVer) throw std::runtime_error("List: bad version");
    }

    uint64_t root_offset() const noexcept { return root_off_; }
    size_t size() const noexcept { return root()->size; }
    bool empty() const noexcept { return size() == 0; }

    class iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        iterator() = default;
        iterator(List* list, uint64_t off) : list_(list), off_(off) {}

        reference operator*() const { return list_->at_node(off_)->value; }
        pointer operator->() const { return &list_->at_node(off_)->value; }
        iterator& operator++() {
            if (off_) off_ = list_->at_node(off_)->next_off;
            return *this;
        }
        iterator operator++(int) { auto t = *this; ++(*this); return t; }
        iterator& operator--() {
            if (!off_) off_ = list_->root()->tail_off;
            else off_ = list_->at_node(off_)->prev_off;
            return *this;
        }
        iterator operator--(int) { auto t = *this; --(*this); return t; }
        bool operator==(const iterator& other) const {
            return list_ == other.list_ && off_ == other.off_;
        }
        bool operator!=(const iterator& other) const { return !(*this == other); }

    private:
        friend class List;
        friend class const_iterator;
        List* list_ = nullptr;
        uint64_t off_ = 0;
    };

    class const_iterator {
        const_iterator(const iterator& it) : list_(it.list_), off_(it.off_) {}
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() = default;
        const_iterator(const List* list, uint64_t off) : list_(list), off_(off) {}

        reference operator*() const { return list_->at_node(off_)->value; }
        pointer operator->() const { return &list_->at_node(off_)->value; }

        const_iterator& operator++() {
            if (off_) off_ = list_->at_node(off_)->next_off;
            return *this;
        }
        const_iterator operator++(int) { auto t = *this; ++(*this); return t; }

        const_iterator& operator--() {
            if (!off_) off_ = list_->root()->tail_off;
            else off_ = list_->at_node(off_)->prev_off;
            return *this;
        }
        const_iterator operator--(int) { auto t = *this; --(*this); return t; }

        bool operator==(const const_iterator& other) const {
            return list_ == other.list_ && off_ == other.off_;
        }
        bool operator!=(const const_iterator& other) const { return !(*this == other); }

    private:
        const List* list_ = nullptr;
        uint64_t off_ = 0;
    };

    iterator begin() noexcept { return iterator(this, root()->head_off); }
    iterator end() noexcept { return iterator(this, 0); }
    const_iterator begin() const noexcept { return const_iterator(this, root()->head_off); }
    const_iterator end() const noexcept { return const_iterator(this, 0); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    void push_back(const T& v) {
        auto* n = make_node(v);
        auto* r = root();
        if (r->tail_off == 0) {
            r->head_off = r->tail_off = off_of(n);
        } else {
            auto* tail = at_node(r->tail_off);
            tail->next_off = off_of(n);
            n->prev_off = r->tail_off;
            r->tail_off = off_of(n);
        }
        ++r->size;
        alloc_->PublishFence();
    }

    void push_front(const T& v) {
        auto* n = make_node(v);
        auto* r = root();
        if (r->head_off == 0) {
            r->head_off = r->tail_off = off_of(n);
        } else {
            auto* head = at_node(r->head_off);
            head->prev_off = off_of(n);
            n->next_off = r->head_off;
            r->head_off = off_of(n);
        }
        ++r->size;
        alloc_->PublishFence();
    }

    void pop_front() {
        auto* r = root();
        if (r->head_off == 0) throw std::out_of_range("List::pop_front");
        auto* head = at_node(r->head_off);
        uint64_t next = head->next_off;
        alloc_->Free(head, FreeMode::Delayed);
        r->head_off = next;
        if (next) at_node(next)->prev_off = 0;
        else r->tail_off = 0;
        --r->size;
        alloc_->PublishFence();
    }

    void pop_back() {
        auto* r = root();
        if (r->tail_off == 0) throw std::out_of_range("List::pop_back");
        auto* tail = at_node(r->tail_off);
        uint64_t prev = tail->prev_off;
        alloc_->Free(tail, FreeMode::Delayed);
        r->tail_off = prev;
        if (prev) at_node(prev)->next_off = 0;
        else r->head_off = 0;
        --r->size;
        alloc_->PublishFence();
    }

    const T& front() const {
        if (root()->head_off == 0) throw std::out_of_range("List::front");
        return at_node(root()->head_off)->value;
    }
    T& front() {
        if (root()->head_off == 0) throw std::out_of_range("List::front");
        return at_node(root()->head_off)->value;
    }
    const T& back() const {
        if (root()->tail_off == 0) throw std::out_of_range("List::back");
        return at_node(root()->tail_off)->value;
    }
    T& back() {
        if (root()->tail_off == 0) throw std::out_of_range("List::back");
        return at_node(root()->tail_off)->value;
    }
    void clear() {
        auto* r = root();
        uint64_t cur = r->head_off;
        while (cur) {
            auto* n = at_node(cur);
            uint64_t next = n->next_off;
            alloc_->Free(n, FreeMode::Delayed);
            cur = next;
        }
        r->head_off = 0;
        r->tail_off = 0;
        r->size = 0;
        alloc_->PublishFence();
    }
    iterator insert(iterator pos, const T& value) {
        if (pos.off_ == 0) {
            push_back(value);
            return iterator(this, root()->tail_off);
        }
        auto* cur = at_node(pos.off_);
        auto* n = make_node(value);
        uint64_t new_off = off_of(n);
        n->prev_off = cur->prev_off;
        n->next_off = pos.off_;
        if (cur->prev_off) at_node(cur->prev_off)->next_off = new_off;
        else root()->head_off = new_off;
        cur->prev_off = new_off;
        ++root()->size;
        alloc_->PublishFence();
        return iterator(this, new_off);
    }
    iterator erase(iterator pos) {
        if (pos.off_ == 0) throw std::out_of_range("List::erase");
        auto* cur = at_node(pos.off_);
        uint64_t prev = cur->prev_off;
        uint64_t next = cur->next_off;
        if (prev) at_node(prev)->next_off = next;
        else root()->head_off = next;
        if (next) at_node(next)->prev_off = prev;
        else root()->tail_off = prev;
        alloc_->Free(cur, FreeMode::Delayed);
        --root()->size;
        alloc_->PublishFence();
        return iterator(this, next);
    }

    template <class Fn>
    void for_each(Fn&& fn) const {
        uint64_t cur = root()->head_off;
        while (cur) {
            const auto* n = at_node(cur);
            fn(n->value);
            cur = n->next_off;
        }
    }

private:
    struct alignas(8) Node {
        uint64_t prev_off;
        uint64_t next_off;
        T value;
    };

    struct alignas(8) Root {
        uint32_t magic;
        uint16_t version;
        uint16_t _pad;
        uint64_t size;
        uint64_t head_off;
        uint64_t tail_off;
    };
    static_assert(sizeof(Root) == 32);

    static constexpr uint32_t kMagic = 0x3154534cu;  // "LST1"
    static constexpr uint16_t kVer   = 1;

    Allocator*   alloc_    = nullptr;
    void* base_            = nullptr;
    uint64_t root_off_     = 0;

    Root* root() noexcept {
        return reinterpret_cast<Root*>(static_cast<char*>(base_) + root_off_);
    }
    const Root* root() const noexcept {
        return reinterpret_cast<const Root*>(static_cast<const char*>(base_) + root_off_);
    }
    Node* at_node(uint64_t off) noexcept {
        return reinterpret_cast<Node*>(static_cast<char*>(base_) + off);
    }
    const Node* at_node(uint64_t off) const noexcept {
        return reinterpret_cast<const Node*>(static_cast<const char*>(base_) + off);
    }
    uint64_t off_of(const void* p) const noexcept {
        return static_cast<uint64_t>(
            static_cast<const char*>(p) - static_cast<const char*>(base_));
    }

    Node* make_node(const T& v) {
        auto* n = static_cast<Node*>(alloc_->Malloc(sizeof(Node)));
        n->prev_off = 0;
        n->next_off = 0;
        n->value = v;
        return n;
    }
};

}  // namespace container
}  // namespace yikv
