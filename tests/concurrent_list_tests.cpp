#include "AtomicMarkableReference.hpp"
#include "CoarseGrainedList.hpp"
#include "FineGrainedList.hpp"
#include "LazyList.hpp"
#include "LockFreeList.hpp"
#include "OptimisticList.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#define CHECK(condition) check((condition), #condition, __LINE__)

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
    }
}

class CoutCapture {
    std::ostringstream stream;
    std::streambuf *previous;

   public:
    CoutCapture() : previous(std::cout.rdbuf(stream.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(previous); }
    std::string str() const { return stream.str(); }
};

template <class Function>
void parallel(int count, Function function) {
    std::atomic<bool> start(false);
    std::vector<std::exception_ptr> errors(count);
    std::vector<std::thread> threads;
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&, i] {
            while (!start.load()) {
                std::this_thread::yield();
            }
            try {
                function(i);
            } catch (...) {
                errors[i] = std::current_exception();
            }
        });
    }
    start.store(true);
    for (auto &thread : threads) {
        thread.join();
    }
    for (const auto &error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
}

class Barrier {
    std::mutex mutex;
    std::condition_variable changed;
    const int participants;
    int arrived = 0;
    int generation = 0;

   public:
    explicit Barrier(int participants) : participants(participants) {}
    void wait() {
        std::unique_lock<std::mutex> guard(mutex);
        const int current = generation;
        if (++arrived == participants) {
            arrived = 0;
            ++generation;
            changed.notify_all();
        } else {
            changed.wait(guard, [&] { return generation != current; });
        }
    }
};

struct Key {
    static int live;
    static int copies;
    static int defaults;
    static int comparisons;
    static bool fail_output;
    int value;

    static void fail_after(int &remaining) {
        if (remaining == 0) {
            throw std::runtime_error("injected key failure");
        }
        if (remaining > 0) {
            --remaining;
        }
    }
    Key() : value(0) {
        fail_after(defaults);
        ++live;
    }
    explicit Key(int value) : value(value) { ++live; }
    Key(const Key &other) : value(other.value) {
        fail_after(copies);
        ++live;
    }
    ~Key() { --live; }
    Key &operator=(const Key &) = delete;
    bool operator<(const Key &other) const {
        fail_after(comparisons);
        return value < other.value;
    }
    bool operator>=(const Key &other) const {
        fail_after(comparisons);
        return value >= other.value;
    }
    bool operator==(const Key &other) const {
        fail_after(comparisons);
        return value == other.value;
    }
    friend std::ostream &operator<<(std::ostream &stream, const Key &key) {
        if (fail_output) {
            throw std::runtime_error("injected output failure");
        }
        return stream << key.value;
    }
};
int Key::live = 0;
int Key::copies = -1;
int Key::defaults = -1;
int Key::comparisons = -1;
bool Key::fail_output = false;

template <class Function>
void expect_key_failure(Function function) {
    bool threw = false;
    try {
        function();
    } catch (const std::runtime_error &) {
        threw = true;
    }
    Key::copies = Key::defaults = Key::comparisons = -1;
    Key::fail_output = false;
    CHECK(threw);
}

void test_atomic() {
    char first = 1;
    char second = 2;
    AtomicMarkableReference<char> reference;
    bool marked = true;
    CHECK(reference.get(&marked) == nullptr);
    CHECK(!marked);
    reference.set(&first, false);
    CHECK(!reference.CAS(&second, nullptr, false, true));
    CHECK(!reference.CAS(&first, nullptr, true, false));
    CHECK(reference.CAS(&first, &first, false, false));
    CHECK(reference.attemptMark(&first, true));
    CHECK(reference.attemptMark(&first, true));
    CHECK(!reference.attemptMark(&second, false));
    CHECK(reference.get(&marked) == &first && marked);
    CHECK(reference.CAS(&first, &second, true, false));
    const auto &constant = reference;
    CHECK(constant.getReference() == &second);
    const MarkableReference<char> pair(&second, false);
    CHECK(pair == MarkableReference<char>(&second, false));

    // Readers must observe a single pointer/mark pair, never a torn snapshot.
    reference.set(&first, false);
    parallel(4, [&](int thread) {
        for (int i = 0; i < 3000; ++i) {
            if (thread < 2) {
                reference.set((i % 2) ? &first : &second, i % 2 == 0);
            } else {
                bool mark = false;
                char *value = reference.get(&mark);
                CHECK((value == &first && !mark) || (value == &second && mark));
            }
        }
    });

    // A mark racing with a successful pointer CAS must not restore the old pointer.
    Barrier barrier(3);
    bool replaced = false;
    bool preserved = true;
    std::thread marker([&] {
        for (int i = 0; i < 2000; ++i) {
            barrier.wait();
            reference.attemptMark(&first, true);
            barrier.wait();
        }
    });
    std::thread replacer([&] {
        for (int i = 0; i < 2000; ++i) {
            barrier.wait();
            replaced = reference.CAS(&first, &second, false, false);
            barrier.wait();
        }
    });
    for (int i = 0; i < 2000; ++i) {
        reference.set(&first, false);
        barrier.wait();
        barrier.wait();
        preserved = preserved && (!replaced || reference.getReference() == &second);
    }
    marker.join();
    replacer.join();
    CHECK(preserved);
}

template <class T>
void print_sequence(CoarseGrainedList<T> &list) {
    std::cout << list;
}

template <class T>
void print_sequence(FineGrainedList<T> &list) {
    list.print_forwards();
    list.print_backwards();
}

template <template <class> class List>
void test_sequence() {
    static_assert(!std::is_copy_constructible<List<int>>::value, "lists own their nodes");
    List<int> list;
    CHECK(list.empty());
    CHECK(list.size() == 0);
    CHECK(list.front() == 0 && list.back() == 0);
    list.pop_back();
    std::vector<int> expected;
    std::mt19937 random(19);
    for (int i = 0; i < 1500; ++i) {
        if (random() % 2) {
            const int key = static_cast<int>(random() % 31) - 15;
            list.push_back(key);
            expected.push_back(key);
        } else {
            list.pop_back();
            if (!expected.empty()) {
                expected.pop_back();
            }
        }
        CHECK(static_cast<std::size_t>(list.size()) == expected.size());
        CHECK(list.empty() == expected.empty());
        CHECK(list.front() == (expected.empty() ? 0 : expected.front()));
        CHECK(list.back() == (expected.empty() ? 0 : expected.back()));
    }
    while (!expected.empty()) {
        list.pop_back();
        expected.pop_back();
    }
    CHECK(list.empty());

    parallel(4, [&](int thread) {
        for (int i = 0; i < 400; ++i) {
            list.push_back(thread + 1);
        }
    });
    CHECK(list.size() == 1600);
    parallel(4, [&](int) {
        for (int i = 0; i < 400; ++i) {
            list.pop_back();
        }
    });
    CHECK(list.empty() && list.size() == 0);

    // Exercise readers while the sentinel links repeatedly become empty.
    parallel(3, [&](int thread) {
        for (int i = 0; i < 2000; ++i) {
            if (thread == 0) {
                list.push_back(1);
                list.pop_back();
            } else {
                const int front = list.front();
                const int back = list.back();
                const auto size = list.size();
                CHECK(front == 0 || front == 1);
                CHECK(back == 0 || back == 1);
                CHECK(size == 0 || size == 1);
                list.empty();
            }
        }
    });
    CHECK(list.empty() && list.size() == 0);

    for (int i = 0; i < 20; ++i) {
        list.push_back(1);
    }
    {
        CoutCapture output;
        parallel(5, [&](int thread) {
            for (int i = 0; i < 1000; ++i) {
                if (thread == 0) {
                    CHECK(list.front() >= 1 && list.front() <= 5);
                    CHECK(list.back() >= 1 && list.back() <= 5);
                    CHECK(list.size() >= 20);
                    CHECK(!list.empty());
                    if (i % 50 == 0) {
                        print_sequence(list);
                    }
                } else {
                    list.push_back(thread + 1);
                    list.pop_back();
                }
            }
        });
    }
    CHECK(list.size() == 20);

    CHECK(Key::live == 0);
    {
        List<Key> keys;
        Key key(7);
        Key::copies = 0;
        expect_key_failure([&] { keys.push_back(key); });
        CHECK(keys.empty());
        keys.push_back(key);
        Key::copies = 0;
        expect_key_failure([&] { keys.front(); });
        Key::copies = 0;
        expect_key_failure([&] { keys.back(); });
        {
            CoutCapture output;
            Key::fail_output = true;
            expect_key_failure([&] { print_sequence(keys); });
        }
        keys.pop_back();
        CHECK(keys.empty());
        keys.push_back(key); // Exercise destruction of a nonempty list.
    }
    CHECK(Key::live == 0);
}

template <template <class> class List>
void test_sentinel_construction_failure() {
    CHECK(Key::live == 0);
    Key::defaults = 1;
    expect_key_failure([] { List<Key> list; });
    CHECK(Key::live == 0);
}

void test_coarse() {
    test_sequence<CoarseGrainedList>();
    CoarseGrainedList<std::string> list;
    list.push_back("a");
    list.push_back("b");
    std::ostringstream output;
    output << list;
    CHECK(output.str() == "a b ");
}

void test_fine() {
    test_sequence<FineGrainedList>();
    test_sentinel_construction_failure<FineGrainedList>();
    {
        FineGrainedList<Key> keys;
        keys.push_back(Key(1));
        CoutCapture output;
        Key::copies = 0;
        expect_key_failure([&] { keys.print_backwards(); });
        Key::fail_output = true;
        expect_key_failure([&] { keys.print_backwards(); });
        keys.pop_back();
        CHECK(keys.empty());
    }
    CHECK(Key::live == 0);
    FineGrainedList<std::string> list;
    {
        CoutCapture output;
        list.print_forwards();
        list.print_backwards();
        CHECK(output.str() == "\n\n");
    }
    list.push_back("a");
    list.push_back("b");
    {
        CoutCapture output;
        list.print_forwards();
        list.print_backwards();
        CHECK(output.str() == "a b \nb a \n");
    }
}

template <template <class> class List, class T>
void test_ordered_keys(const std::vector<T> &keys) {
    List<T> list;
    for (const T &key : keys) {
        CHECK(!list.contains(key));
        CHECK(!list.remove(key));
    }
    for (const T &key : keys) {
        CHECK(list.add(key));
        CHECK(list.contains(key));
        CHECK(!list.add(key));
    }
    for (const T &key : keys) {
        CHECK(list.remove(key));
        CHECK(!list.contains(key));
        CHECK(!list.remove(key));
    }
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (const T &key : keys) {
            CHECK(list.add(key));
        }
        list.deleteList();
        list.deleteList();
        for (const T &key : keys) {
            CHECK(!list.contains(key));
            CHECK(!list.remove(key));
        }
    }
}

// Existing lazy/optimistic key types need not define operator<.
struct LegacyKey {
    int value;
    LegacyKey() : value(0) {}
    explicit LegacyKey(int value) : value(value) {}
    bool operator>=(const LegacyKey &other) const { return value >= other.value; }
    bool operator>(const LegacyKey &other) const { return value > other.value; }
    bool operator==(const LegacyKey &other) const { return value == other.value; }
    bool operator!=(const LegacyKey &other) const { return value != other.value; }
};

struct Operation {
    int kind;
    int key;
    int start;
    int finish;
    bool result;
};

// Find a legal sequential history that preserves non-overlapping call order.
bool linearizable(const std::vector<Operation> &history, int done, int state,
                  int final_state, std::vector<bool> &visited) {
    const int all = (1 << history.size()) - 1;
    if (done == all) {
        return state == final_state;
    }
    const int cache_key = done * 4 + state;
    if (visited[cache_key]) {
        return false;
    }
    visited[cache_key] = true;
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (done & (1 << i)) {
            continue;
        }
        bool ready = true;
        for (std::size_t j = 0; j < history.size(); ++j) {
            if (!(done & (1 << j)) && history[j].finish < history[i].start) {
                ready = false;
                break;
            }
        }
        if (!ready) {
            continue;
        }
        const Operation &operation = history[i];
        const int bit = 1 << operation.key;
        const bool present = (state & bit) != 0;
        const bool result = operation.kind == 0 ? !present : present;
        if (result != operation.result) {
            continue;
        }
        int next = state;
        if (operation.kind == 0) {
            next |= bit;
        } else if (operation.kind == 1) {
            next &= ~bit;
        }
        if (linearizable(history, done | (1 << i), next, final_state, visited)) {
            return true;
        }
    }
    return false;
}

template <template <class> class List>
void test_ordered_concurrency() {
    List<int> list;
    for (int round = 0; round < 12; ++round) {
        std::atomic<int> additions(0);
        std::atomic<int> removals(0);
        parallel(6, [&](int) {
            for (int key = -8; key <= 8; ++key) {
                if (list.add(key)) {
                    ++additions;
                }
                CHECK(list.contains(key));
            }
        });
        CHECK(additions.load() == 17);
        parallel(6, [&](int) {
            for (int key = -8; key <= 8; ++key) {
                if (list.remove(key)) {
                    ++removals;
                }
            }
        });
        CHECK(removals.load() == 17);
        for (int key = -8; key <= 8; ++key) {
            CHECK(!list.contains(key));
        }
    }
    CHECK(list.add(10000));
    {
        CoutCapture output;
        parallel(7, [&](int thread) {
            for (int i = 0; i < 1200; ++i) {
                if (thread == 6) {
                    list.printList();
                } else {
                    const int key = thread * 8 + i % 8 - 24;
                    CHECK(list.add(key));
                    CHECK(list.contains(key));
                    CHECK(list.contains(10000));
                    CHECK(list.remove(key));
                    CHECK(!list.contains(key));
                }
            }
        });
    }
    CHECK(list.remove(10000));
    list.deleteList();

    std::mt19937 random(27);
    for (int round = 0; round < 60; ++round) {
        List<int> checked;
        std::vector<Operation> history(9);
        for (auto &operation : history) {
            operation.kind = random() % 3;
            operation.key = random() % 2;
        }
        std::atomic<int> clock(0);
        parallel(3, [&](int thread) {
            for (int i = 0; i < 3; ++i) {
                Operation &operation = history[thread * 3 + i];
                operation.start = clock.fetch_add(1);
                std::this_thread::yield();
                if (operation.kind == 0) {
                    operation.result = checked.add(operation.key);
                } else if (operation.kind == 1) {
                    operation.result = checked.remove(operation.key);
                } else {
                    operation.result = checked.contains(operation.key);
                }
                operation.finish = clock.fetch_add(1);
            }
        });
        const int final_state = (checked.contains(0) ? 1 : 0) | (checked.contains(1) ? 2 : 0);
        std::vector<bool> visited((1 << history.size()) * 4, false);
        CHECK(linearizable(history, 0, 0, final_state, visited));
    }
}

template <template <class> class List>
void test_ordered() {
    static_assert(!std::is_copy_constructible<List<int>>::value, "lists own their nodes");
    static_assert(!std::is_copy_assignable<List<int>>::value, "lists own their nodes");
    test_ordered_keys<List, int>({0, -1, 1, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()});
    test_ordered_keys<List, long long>({0, std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max()});
    test_ordered_keys<List, unsigned>({0, 1, std::numeric_limits<unsigned>::max()});
    test_ordered_keys<List, std::string>({"", "z", "a", "hello"});
    test_sentinel_construction_failure<List>();

    List<int> list;
    std::set<int> model;
    std::mt19937 random(42);
    for (int i = 0; i < 2000; ++i) {
        const int key = static_cast<int>(random() % 31) - 15;
        switch (random() % 3) {
            case 0: CHECK(list.add(key) == model.insert(key).second); break;
            case 1: CHECK(list.remove(key) == (model.erase(key) != 0)); break;
            default: CHECK(list.contains(key) == (model.count(key) != 0)); break;
        }
    }
    {
        CoutCapture output;
        list.printList();
        std::ostringstream expected;
        for (int key : model) {
            expected << key << " ";
        }
        CHECK(output.str() == expected.str());
    }
    list.deleteList();
    {
        CoutCapture output;
        list.printList();
        CHECK(output.str().empty());
    }

    CHECK(Key::live == 0);
    {
        List<Key> keys;
        Key key(2);
        Key::copies = 1; // Parameter copy succeeds; node construction throws.
        expect_key_failure([&] { keys.add(key); });
        CHECK(!keys.contains(key));
        CHECK(keys.add(key));
        Key::comparisons = 1; // Equality throws after traversal, with update locks held.
        expect_key_failure([&] { keys.add(key); });
        CHECK(keys.contains(key));
        {
            CoutCapture output;
            Key::fail_output = true;
            expect_key_failure([&] { keys.printList(); });
        }
        CHECK(keys.remove(key));
        keys.deleteList();
        CHECK(Key::live == 3); // Two sentinels and the local key; removed nodes reclaimed.
        CHECK(keys.add(key));
        CHECK(keys.remove(key)); // Also reclaim removed nodes during destruction.
    }
    CHECK(Key::live == 0);
    test_ordered_concurrency<List>();
}

int main(int argc, char **argv) {
    const std::string selected = argc == 2 ? argv[1] : "all";
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"atomic", test_atomic},
        {"coarse", test_coarse},
        {"fine", test_fine},
        {"optimistic", [] {
            test_ordered<OptimisticList>();
            test_ordered_keys<OptimisticList, LegacyKey>({LegacyKey(-1), LegacyKey(0), LegacyKey(1)});
        }},
        {"lazy", [] {
            test_ordered<LazyList>();
            test_ordered_keys<LazyList, LegacyKey>({LegacyKey(-1), LegacyKey(0), LegacyKey(1)});
        }},
        {"lock_free", test_ordered<LockFreeList>},
    };
    bool found = false;
    for (const auto &test : tests) {
        if (selected != "all" && selected != test.first) {
            continue;
        }
        found = true;
        try {
            test.second();
            std::cout << test.first << ": PASS" << std::endl;
        } catch (const std::exception &error) {
            std::cerr << test.first << ": FAIL: " << error.what() << std::endl;
            return 1;
        }
    }
    if (!found) {
        std::cerr << "Unknown test group: " << selected << std::endl;
        return 1;
    }
}
