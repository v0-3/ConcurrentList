#pragma once

#include <atomic>
#include <memory>

template <class T>
struct MarkableReference {
    T *next;
    bool marked;

    MarkableReference() : next(nullptr), marked(false) {}
    MarkableReference(T *node, bool mark) : next(node), marked(mark) {}

    bool operator==(const MarkableReference<T> &other) const {
        return next == other.next && marked == other.marked;
    }
};

template <class T>
class AtomicMarkableReference {
   private:
    struct State : MarkableReference<T> {
        State *previous;
        State(T *node, bool mark, State *previous)
            : MarkableReference<T>(node, mark), previous(previous) {}
    };

    // Immutable snapshots keep concurrent readers safe and prevent pointer ABA.
    // Reclaim the history only when no callers can still hold a snapshot.
    std::atomic<State *> markedNext;

   public:
    AtomicMarkableReference() : markedNext(new State(nullptr, false, nullptr)) {}
    AtomicMarkableReference(T *nextNode, bool mark)
        : markedNext(new State(nextNode, mark, nullptr)) {}

    ~AtomicMarkableReference() {
        State *curr = markedNext.load();
        while (curr) {
            State *previous = curr->previous;
            delete curr;
            curr = previous;
        }
    }

    AtomicMarkableReference(const AtomicMarkableReference &) = delete;
    AtomicMarkableReference &operator=(const AtomicMarkableReference &) = delete;

    T *getReference() const {
        return markedNext.load()->next;
    }

    T *get(bool *mark) const {
        State *curr = markedNext.load();
        *mark = curr->marked;
        return curr->next;
    }

    void set(T *newRef, bool newMark) {
        State *curr = markedNext.load();
        if (newRef == curr->next && newMark == curr->marked) {
            return;
        }
        std::unique_ptr<State> next(new State(newRef, newMark, curr));
        do {
            if (newRef == curr->next && newMark == curr->marked) {
                return;
            }
            next->previous = curr;
        } while (!markedNext.compare_exchange_weak(curr, next.get()));
        next.release();
    }

    // A failed CAS must not overwrite a concurrently changed reference.
    bool attemptMark(T *expected, bool newMark) {
        State *curr = markedNext.load();
        if (expected != curr->next) {
            return false;
        }
        if (newMark == curr->marked) {
            return true;
        }
        std::unique_ptr<State> next(new State(expected, newMark, curr));
        if (!markedNext.compare_exchange_strong(curr, next.get())) {
            return false;
        }
        next.release();
        return true;
    }

    bool CAS(T *expected, T *newValue, bool expectedBool, bool newBool) {
        State *curr = markedNext.load();
        if (expected != curr->next || expectedBool != curr->marked) {
            return false;
        }
        if (newValue == curr->next && newBool == curr->marked) {
            return true;
        }
        std::unique_ptr<State> next(new State(newValue, newBool, curr));
        if (!markedNext.compare_exchange_strong(curr, next.get())) {
            return false;
        }
        next.release();
        return true;
    }
};
