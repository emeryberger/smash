// smash/src/util/intrusive_list.h - Intrusive doubly-linked list
#pragma once

#include <cstddef>

namespace smash {

// Nodes must have list_prev and list_next pointer members of type T*.
// A node is in at most one list at a time.
template<typename T>
class IntrusiveList {
    T* head_ = nullptr;
    T* tail_ = nullptr;
    size_t size_ = 0;

public:
    bool empty() const { return head_ == nullptr; }
    size_t size() const { return size_; }
    T* front() const { return head_; }
    T* back() const { return tail_; }

    void pushFront(T* node) {
        node->list_prev = nullptr;
        node->list_next = head_;
        if (head_) {
            head_->list_prev = node;
        } else {
            tail_ = node;
        }
        head_ = node;
        ++size_;
    }

    void pushBack(T* node) {
        node->list_next = nullptr;
        node->list_prev = tail_;
        if (tail_) {
            tail_->list_next = node;
        } else {
            head_ = node;
        }
        tail_ = node;
        ++size_;
    }

    T* popFront() {
        if (!head_) return nullptr;
        T* node = head_;
        head_ = node->list_next;
        if (head_) {
            head_->list_prev = nullptr;
        } else {
            tail_ = nullptr;
        }
        node->list_next = nullptr;
        node->list_prev = nullptr;
        --size_;
        return node;
    }

    void remove(T* node) {
        if (node->list_prev) {
            node->list_prev->list_next = node->list_next;
        } else {
            head_ = node->list_next;
        }
        if (node->list_next) {
            node->list_next->list_prev = node->list_prev;
        } else {
            tail_ = node->list_prev;
        }
        node->list_prev = nullptr;
        node->list_next = nullptr;
        --size_;
    }
};

} // namespace smash
