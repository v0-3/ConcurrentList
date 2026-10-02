/*************************************************************************
 * Luis Maya Aranda
 *
 * Optimistic Synchronization Linked List
 * Search without locking, lock the adjacent nodes, then validate that
 * they are still adjacent and reachable before completing an operation.
 *
 * **********************************************************************/
#pragma once

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>

template <class T>
class OptimisticList {
   public:
    OptimisticList();
    ~OptimisticList();
    OptimisticList(const OptimisticList &) = delete;
    OptimisticList &operator=(const OptimisticList &) = delete;
    bool contains(T);
    bool add(T);
    bool remove(T);
    void printList();
    // Requires no concurrent operations, just like destruction.
    void deleteList();

   private:
    struct Node {
        T key;
        std::atomic<Node *> next;
        std::mutex lock;
        Node *ownedNext;

        Node() : key(), next(nullptr), ownedNext(nullptr) {}
        Node(const T &key, Node *next) : key(key), next(next), ownedNext(nullptr) {}
    };
    Node *head;
    Node *tail;
    // Keep unlinked nodes alive for readers until a quiescent clear/destruction.
    std::atomic<Node *> owned;
    void own(Node *);
    bool validate(Node *, Node *);
};

template <class T>
OptimisticList<T>::OptimisticList() : owned(nullptr) {
    std::unique_ptr<Node> first(new Node());
    std::unique_ptr<Node> last(new Node());
    first->next.store(last.get());
    head = first.release();
    tail = last.release();
}

template <class T>
OptimisticList<T>::~OptimisticList() {
    deleteList();
    delete head;
    delete tail;
}

template <class T>
bool OptimisticList<T>::contains(T key) {
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
            return curr != tail && curr->key == key;
        }
    }
}

template <class T>
bool OptimisticList<T>::add(T key) {
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
bool OptimisticList<T>::remove(T key) {
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
            pred->next.store(curr->next.load());
            return true;
        }
    }
}

template <class T>
void OptimisticList<T>::printList() {
    // Concurrent updates may be observed; this is not an atomic snapshot.
    Node *curr = head->next.load();
    while (curr != tail) {
        std::cout << curr->key << " ";
        curr = curr->next.load();
    }
}

template <class T>
bool OptimisticList<T>::validate(Node *pred, Node *curr) {
    Node *node = head;
    while (node != tail) {
        if (node == pred) {
            return pred->next.load() == curr;
        }
        node = node->next.load();
    }
    return false;
}

template <class T>
void OptimisticList<T>::own(Node *node) {
    node->ownedNext = owned.load();
    while (!owned.compare_exchange_weak(node->ownedNext, node)) {
    }
}

template <class T>
void OptimisticList<T>::deleteList() {
    head->next.store(tail);
    Node *curr = owned.exchange(nullptr);
    while (curr) {
        Node *next = curr->ownedNext;
        delete curr;
        curr = next;
    }
}
