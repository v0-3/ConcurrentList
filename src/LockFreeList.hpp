/*************************************************************************
 * Luis Maya Aranda
 *
 * Lock-free Linked List
 *
 * **********************************************************************/
#pragma once

#include <iostream>
#include <memory>
#include <new>

#include "AtomicMarkableReference.hpp"

template <class T>
class LockFreeList {
   public:
    LockFreeList();
    ~LockFreeList();
    LockFreeList(const LockFreeList &) = delete;
    LockFreeList &operator=(const LockFreeList &) = delete;
    bool contains(T);
    bool add(T);
    bool remove(T);
    void printList();
    // Requires no concurrent operations, just like destruction.
    void deleteList();

   private:
    struct Node {
        T key;
        AtomicMarkableReference<Node> next;
        Node *ownedNext;

        Node() : key(), next(), ownedNext(nullptr) {}
        Node(const T &key, Node *next)
            : key(key), next(next, false), ownedNext(nullptr) {}
    };

    struct Window {
        Node *pred;
        Node *curr;

        // Find adjacent nodes, helping unlink any logically removed nodes.
        Window(Node *head, Node *tail, const T &key) {
        RETRY:
            pred = head;
            curr = pred->next.getReference();
            while (true) {
                bool marked = false;
                Node *succ = curr->next.get(&marked);
                while (marked) {
                    if (!pred->next.CAS(curr, succ, false, false)) {
                        goto RETRY;
                    }
                    curr = succ;
                    succ = curr->next.get(&marked);
                }
                if (curr == tail || curr->key >= key) {
                    return;
                }
                pred = curr;
                curr = succ;
            }
        }
    };
    Node *head;
    Node *tail;
    // Own every published node, including nodes unlinked by helper threads.
    // Readers may still hold those nodes until a quiescent clear/destruction.
    std::atomic<Node *> owned;
    void own(Node *);
    void delete_nodes();
};

template <class T>
LockFreeList<T>::LockFreeList() : owned(nullptr) {
    std::unique_ptr<Node> first(new Node());
    std::unique_ptr<Node> last(new Node());
    first->next.set(last.get(), false);
    head = first.release();
    tail = last.release();
}

template <class T>
LockFreeList<T>::~LockFreeList() {
    delete_nodes();
    delete head;
    delete tail;
}

template <class T>
bool LockFreeList<T>::contains(T key) {
    Node *curr = head->next.getReference();
    while (curr != tail && curr->key < key) {
        curr = curr->next.getReference();
    }
    if (curr == tail || !(curr->key == key)) {
        return false;
    }
    bool marked = false;
    curr->next.get(&marked);
    return !marked;
}

template <class T>
bool LockFreeList<T>::add(T key) {
    while (true) {
        Window window(head, tail, key);
        Node *pred = window.pred;
        Node *curr = window.curr;
        if (curr != tail && curr->key == key) {
            return false;
        }
        std::unique_ptr<Node> node(new Node(key, curr));
        if (pred->next.CAS(curr, node.get(), false, false)) {
            own(node.release());
            return true;
        }
    }
}

template <class T>
bool LockFreeList<T>::remove(T key) {
    while (true) {
        Window window(head, tail, key);
        Node *pred = window.pred;
        Node *curr = window.curr;
        if (curr == tail || !(curr->key == key)) {
            return false;
        }
        Node *succ = curr->next.getReference();
        // Only the thread that changes false to true successfully removes it.
        if (!curr->next.CAS(succ, succ, false, true)) {
            continue;
        }
        try {
            pred->next.CAS(curr, succ, false, false);
        } catch (const std::bad_alloc &) {
            // Removal already committed; a later search can finish unlinking.
        }
        return true;
    }
}

template <class T>
void LockFreeList<T>::printList() {
    // Concurrent updates may be observed; this is not an atomic snapshot.
    Node *curr = head->next.getReference();
    while (curr != tail) {
        bool marked = false;
        Node *next = curr->next.get(&marked);
        if (!marked) {
            std::cout << curr->key << " ";
        }
        curr = next;
    }
}

template <class T>
void LockFreeList<T>::own(Node *node) {
    node->ownedNext = owned.load();
    while (!owned.compare_exchange_weak(node->ownedNext, node)) {
    }
}

template <class T>
void LockFreeList<T>::delete_nodes() {
    Node *curr = owned.exchange(nullptr);
    while (curr) {
        Node *next = curr->ownedNext;
        delete curr;
        curr = next;
    }
}

template <class T>
void LockFreeList<T>::deleteList() {
    // Replacing the head also reclaims its accumulated atomic snapshots.
    // Allocate first so a failed allocation leaves the original list intact.
    std::unique_ptr<Node> first(new Node());
    first->next.set(tail, false);
    delete_nodes();
    delete head;
    head = first.release();
}
