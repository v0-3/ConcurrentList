/*************************************************************************
 * Luis Maya Aranda
 * Coarse-Grained Synchronization List
 *
 * The list itself has a single lock which every method call must acquire.
 * The principle advantage of this algorithm, is that it is thread safe.
 * All methods act on the list only while holding the lock, so the execution
 * is essentially sequential.
 *
 * **********************************************************************/
#pragma once

#include <cstddef>
#include <mutex>
#include <ostream>

template <class T>
class CoarseGrainedList {
   private:
    struct Node {
        T key;
        Node *prev;
        Node *next;

        Node() : key(T()), prev(nullptr), next(nullptr) {}
        Node(T key) : key(key), prev(nullptr), next(nullptr) {}
        Node(T key, Node *prev, Node *next) : key(key), prev(prev), next(next) {}
    };
    Node *head;
    Node *tail;
    std::size_t list_size;
    mutable std::mutex lock;
    void delete_list() {
        std::lock_guard<std::mutex> guard(lock);

        Node *itr = head;
        while (itr) {
            Node *next = itr->next;
            delete itr;
            itr = next;
        }
        head = nullptr;
        tail = nullptr;
        list_size = 0;
    }

   public:
    CoarseGrainedList() : head(nullptr), tail(nullptr), list_size(0) {}

    ~CoarseGrainedList() {
        delete_list();
    }

    T front() const {
        std::lock_guard<std::mutex> guard(lock);
        return head != nullptr ? head->key : T();
    }

    T back() const {
        std::lock_guard<std::mutex> guard(lock);
        return tail != nullptr ? tail->key : T();
    }

    bool empty() const {
        std::lock_guard<std::mutex> guard(lock);
        return head == nullptr && tail == nullptr;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> guard(lock);
        return list_size;
    }
    void push_back(const T &key) {
        std::lock_guard<std::mutex> guard(lock);

        Node *node = new Node(key);
        if (head == nullptr) {
            head = node;
            tail = node;
        } else {
            tail->next = node;
            node->prev = tail;
            tail = node;
        }
        list_size++;
    }

    void pop_back() {
        std::lock_guard<std::mutex> guard(lock);

        if (head && tail) {
            if (head == tail) {
                delete head;
                head = nullptr;
                tail = nullptr;
            } else {
                Node *itr = tail;
                tail = tail->prev;
                tail->next = nullptr;
                delete itr;
            }
            list_size--;
        }
    }

    friend std::ostream &operator<<(std::ostream &os, const CoarseGrainedList &list) {
        std::lock_guard<std::mutex> guard(list.lock);

        Node *itr = list.head;
        while (itr) {
            os << itr->key << " ";
            itr = itr->next;
        }
        return os;
    }
};
