#include "AtomicMarkableReference.hpp"
#include "CoarseGrainedList.hpp"
#include "FineGrainedList.hpp"
#include "LazyList.hpp"
#include "LockFreeList.hpp"
#include "OptimisticList.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {
std::atomic<std::ptrdiff_t> outstanding(0);
std::atomic<int> allocations_before_failure(-1);
}

// Isolate allocation accounting/fault injection from the threaded test binary.
void *operator new(std::size_t size) {
    if (allocations_before_failure.load() >= 0 && allocations_before_failure.fetch_sub(1) == 0) {
        throw std::bad_alloc();
    }
    void *pointer = std::malloc(size == 0 ? 1 : size);
    if (!pointer) {
        throw std::bad_alloc();
    }
    ++outstanding;
    return pointer;
}

void operator delete(void *pointer) noexcept {
    if (pointer) {
        --outstanding;
        std::free(pointer);
    }
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void *pointer) noexcept { ::operator delete(pointer); }
#if __cplusplus >= 201402L
void operator delete(void *pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { ::operator delete(pointer); }
#endif

#define CHECK(condition) check((condition), #condition, __LINE__)

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "line %d: %s\n", line, expression);
        throw std::runtime_error("allocation test failed");
    }
}

template <class Function>
bool inject_failure(int after, Function function) {
    allocations_before_failure.store(after);
    try {
        function();
    } catch (const std::bad_alloc &) {
        allocations_before_failure.store(-1);
        return true;
    } catch (...) {
        allocations_before_failure.store(-1);
        throw;
    }
    allocations_before_failure.store(-1);
    return false;
}

template <class Function>
void no_leaks(Function function) {
    const auto before = outstanding.load();
    function();
    CHECK(outstanding.load() == before);
}

template <template <class> class List>
void test_construction_failures() {
    for (int allocation = 0; allocation < 6; ++allocation) {
        no_leaks([&] {
            inject_failure(allocation, [] { List<int> list; });
        });
    }
}

template <template <class> class List>
void test_sequence_allocations() {
    test_construction_failures<List>();
    for (int allocation = 0; allocation < 4; ++allocation) {
        no_leaks([&] {
            List<int> list;
            list.push_back(1);
            const bool failed = inject_failure(allocation, [&] { list.push_back(2); });
            CHECK(list.size() == (failed ? 1 : 2));
            CHECK(list.front() == 1);
            CHECK(list.back() == (failed ? 1 : 2));
            list.pop_back();
            list.push_back(3);
        });
    }
}

template <template <class> class List>
void test_ordered_allocations() {
    test_construction_failures<List>();
    for (int allocation = 0; allocation < 6; ++allocation) {
        no_leaks([&] {
            List<int> list;
            CHECK(list.add(1));
            const bool failed = inject_failure(allocation, [&] { list.add(2); });
            CHECK(list.contains(1));
            CHECK(list.contains(2) == !failed);
            CHECK(list.remove(1));
            CHECK(list.add(3));
        });
        no_leaks([&] {
            List<int> list;
            CHECK(list.add(1));
            const bool failed = inject_failure(allocation, [&] { list.deleteList(); });
            CHECK(list.contains(1) == failed);
            CHECK(list.add(2));
        });
    }
    no_leaks([] {
        List<int> list;
        const auto empty = outstanding.load();
        for (int round = 0; round < 4; ++round) {
            for (int key = 0; key < 100; ++key) {
                CHECK(list.add(key));
            }
            for (int key = 0; key < 100; key += 2) {
                CHECK(list.remove(key));
            }
            list.deleteList();
            CHECK(outstanding.load() == empty);
        }
        CHECK(list.add(1));
        CHECK(list.remove(1)); // Destruction must own unlinked nodes too.
    });
}

void test_atomic_allocations() {
    test_construction_failures<AtomicMarkableReference>();
    no_leaks([] {
        int first = 1;
        int second = 2;
        AtomicMarkableReference<int> reference(&first, false);
        bool marked = true;
        CHECK(inject_failure(0, [&] { reference.set(&second, true); }));
        CHECK(reference.get(&marked) == &first && !marked);
        CHECK(inject_failure(0, [&] { reference.CAS(&first, &second, false, true); }));
        CHECK(reference.get(&marked) == &first && !marked);
        CHECK(inject_failure(0, [&] { reference.attemptMark(&first, true); }));
        CHECK(reference.get(&marked) == &first && !marked);
        for (int i = 0; i < 1000; ++i) {
            reference.set(&first, false);
            CHECK(reference.CAS(&first, &second, false, true));
            CHECK(reference.attemptMark(&second, false));
        }
    });
}

void test_committed_removal() {
    no_leaks([] {
        LockFreeList<int> list;
        CHECK(list.add(1));
        bool removed = false;
        // Marking allocates one snapshot; the following physical unlink fails.
        CHECK(!inject_failure(1, [&] { removed = list.remove(1); }));
        CHECK(removed);
        CHECK(!list.contains(1));
        CHECK(!list.remove(1)); // A subsequent search helps unlink the node.
        CHECK(list.add(1));
        CHECK(list.remove(1));
    });
}

int main() {
    try {
        test_atomic_allocations();
        test_sequence_allocations<CoarseGrainedList>();
        test_sequence_allocations<FineGrainedList>();
        test_ordered_allocations<LazyList>();
        test_ordered_allocations<OptimisticList>();
        test_ordered_allocations<LockFreeList>();
        test_committed_removal();
        std::puts("allocations: PASS");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "allocations: FAIL: %s\n", error.what());
        return 1;
    }
}
