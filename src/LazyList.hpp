/*************************************************************************
 * Luis Maya Aranda
 *
 * Lazy Synchronization Linked List
 * Traversals use atomic links. Updates lock adjacent nodes and validate
 * that neither has been logically removed before changing their links.
 *
 * **********************************************************************/
#pragma once

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>

template <class T>
class LazyList {
   public:
    LazyList();
    ~LazyList();
    LazyList(const LazyList &) = delete;
    LazyList &operator=(const LazyList &) = delete;
    bool contains(T);
    bool add(T);
    bool remove(T);
    void printList();
    // Requires no concurrent operations, just like destruction.
    void deleteList();

   private:
    struct Node {
        T key;
        std::atomic<bool> marked;
        std::atomic<Node *> next;
        std::mutex lock;
        Node *ownedNext;

        Node() : key(), marked(false), next(nullptr), ownedNext(nullptr) {}
        Node(const T &key, Node *next)
            : key(key), marked(false), next(next), ownedNext(nullptr) {}
    };
    Node *head;
    Node *tail;
    // Keep unlinked nodes alive for readers until a quiescent clear/destruction.
    std::atomic<Node *> owned;
    void own(Node *);
    bool validate(Node *, Node *);
};

template <class T>
LazyList<T>::LazyList() : owned(nullptr) {
    std::unique_ptr<Node> first(new Node());
    std::unique_ptr<Node> last(new Node());
    first->next.store(last.get());
    head = first.release();
    tail = last.release();
}

template <class T>
LazyList<T>::~LazyList() {
    deleteList();
    delete head;
    delete tail;
}

template <class T>
bool LazyList<T>::contains(T key) {
    Node *curr = head->next.load();
    while (curr != tail && !(curr->key >= key)) {
        curr = curr->next.load();
    }
    return curr != tail && curr->key == key && !curr->marked.load();
}

template <class T>
bool LazyList<T>::add(T key) {
    while (true) {
        Node *pred = head;
        Node *curr = pred->next.load();
        while (curr != tail && !(curr->key >= key)) {
            pred = curr;
            curr = curr->next.load();
        }

        std::lock_guard<std::mutex> pred_guard(pred->lock);
        std::lock_guard<std::mutex> curr_guard(curr->lock);
        if (validate(pred, curr)) {
            if (curr != tail && curr->key == key) {
                return false;
            }
            Node *node = new Node(key, curr);
            own(node);
            pred->next.store(node);
            return true;
        }
    }
}

template <class T>
bool LazyList<T>::remove(T key) {
    while (true) {
        Node *pred = head;
        Node *curr = pred->next.load();
        while (curr != tail && !(curr->key >= key)) {
            pred = curr;
            curr = curr->next.load();
        }

        std::lock_guard<std::mutex> pred_guard(pred->lock);
        std::lock_guard<std::mutex> curr_guard(curr->lock);
        if (validate(pred, curr)) {
            if (curr == tail || !(curr->key == key)) {
                return false;
            }
            curr->marked.store(true);
            pred->next.store(curr->next.load());
            return true;
        }
    }
}

template <class T>
void LazyList<T>::printList() {
    // Concurrent updates may be observed; this is not an atomic snapshot.
    Node *curr = head->next.load();
    while (curr != tail) {
        if (!curr->marked.load()) {
            std::cout << curr->key << " ";
        }
        curr = curr->next.load();
    }
}

template <class T>
bool LazyList<T>::validate(Node *pred, Node *curr) {
    return !pred->marked.load() && !curr->marked.load() && pred->next.load() == curr;
}

template <class T>
void LazyList<T>::own(Node *node) {
    node->ownedNext = owned.load();
    while (!owned.compare_exchange_weak(node->ownedNext, node)) {
    }
}

template <class T>
void LazyList<T>::deleteList() {
    head->next.store(tail);
    Node *curr = owned.exchange(nullptr);
    while (curr) {
        Node *next = curr->ownedNext;
        delete curr;
        curr = next;
    }
}
