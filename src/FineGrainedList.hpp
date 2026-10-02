/*************************************************************************
 * Luis Maya Aranda
 *
 * Fine-Grained Synchronization Linked List
 * Forward readers lock consecutive nodes in list order. Writers protect
 * the back endpoint with the tail sentinel's lock, then lock the affected
 * nodes in forward order. Readers never acquire the tail sentinel's lock.
 *
 * **********************************************************************/
#pragma once

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

template <class T>
class FineGrainedList {
   public:
    FineGrainedList();
    ~FineGrainedList();
    T front() const;
    T back() const;
    bool empty() const;
    int size() const;
    void push_back(const T &);
    void pop_back();
    void print_forwards();
    void print_backwards();

   private:
    struct Node {
        T key;
        Node *prev;
        Node *next;
        mutable std::mutex lock;

        Node() : key(T()), prev(nullptr), next(nullptr) {}
        Node(const T &key) : key(key), prev(nullptr), next(nullptr) {}
    };
    std::atomic<int> list_size;
    Node *head;
    Node *tail;
    void delete_list();
};

template <class T>
FineGrainedList<T>::FineGrainedList() : list_size(0) {
    std::unique_ptr<Node> first(new Node());
    std::unique_ptr<Node> last(new Node());
    first->next = last.get();
    last->prev = first.get();
    head = first.release();
    tail = last.release();
}

template <class T>
FineGrainedList<T>::~FineGrainedList() {
    // As with standard containers, destruction requires no concurrent callers.
    delete_list();
}

template <class T>
T FineGrainedList<T>::front() const {
    std::unique_lock<std::mutex> head_guard(head->lock);
    Node *first = head->next;
    if (first == tail) {
        return T();
    }
    std::lock_guard<std::mutex> first_guard(first->lock);
    head_guard.unlock();
    return first->key;
}

template <class T>
T FineGrainedList<T>::back() const {
    std::lock_guard<std::mutex> tail_guard(tail->lock);
    Node *last = tail->prev;
    if (last == head) {
        return T();
    }
    std::lock_guard<std::mutex> last_guard(last->lock);
    return last->key;
}

template <class T>
bool FineGrainedList<T>::empty() const {
    return list_size.load() == 0;
}

template <class T>
int FineGrainedList<T>::size() const {
    return list_size.load();
}

template <class T>
void FineGrainedList<T>::push_back(const T &key) {
    std::unique_ptr<Node> node(new Node(key));
    std::lock_guard<std::mutex> tail_guard(tail->lock);
    Node *last = tail->prev;
    std::lock_guard<std::mutex> last_guard(last->lock);

    node->prev = last;
    node->next = tail;
    last->next = node.get();
    tail->prev = node.release();
    ++list_size;
}

template <class T>
void FineGrainedList<T>::pop_back() {
    std::lock_guard<std::mutex> tail_guard(tail->lock);
    Node *last = tail->prev;
    if (last == head) {
        return;
    }

    Node *pred = last->prev;
    std::lock_guard<std::mutex> pred_guard(pred->lock);
    std::unique_lock<std::mutex> last_guard(last->lock);
    pred->next = tail;
    tail->prev = pred;
    --list_size;
    last_guard.unlock();
    delete last;
}

template <class T>
void FineGrainedList<T>::print_forwards() {
    Node *curr = head;
    std::unique_lock<std::mutex> guard(curr->lock);
    while (curr->next != tail) {
        Node *next = curr->next;
        std::unique_lock<std::mutex> next_guard(next->lock);
        guard.unlock();
        curr = next;
        guard = std::move(next_guard);
        std::cout << curr->key << " ";
    }
    std::cout << std::endl;
}

template <class T>
void FineGrainedList<T>::print_backwards() {
    // Reverse lock acquisition would deadlock with forward readers/writers.
    std::vector<T> keys;
    {
        Node *curr = head;
        std::unique_lock<std::mutex> guard(curr->lock);
        while (curr->next != tail) {
            Node *next = curr->next;
            std::unique_lock<std::mutex> next_guard(next->lock);
            guard.unlock();
            curr = next;
            guard = std::move(next_guard);
            keys.push_back(curr->key);
        }
    }
    for (auto itr = keys.rbegin(); itr != keys.rend(); ++itr) {
        std::cout << *itr << " ";
    }
    std::cout << std::endl;
}

template <class T>
void FineGrainedList<T>::delete_list() {
    Node *curr = head;
    while (curr) {
        Node *next = curr->next;
        delete curr;
        curr = next;
    }
    head = nullptr;
    tail = nullptr;
    list_size = 0;
}
