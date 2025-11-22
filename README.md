<br>

<p align="center">
  <a href="https://github.com/v0-3">
    <img width="200" src="./logo/set.png" alt="ConcurrentList logo">
  </a>
</p>

<br>

# ConcurrentList

**ConcurrentList** is a collection of concurrent linked-list implementations in modern C++.  
Each implementation demonstrates a different synchronization technique, ranging from coarse-grained locks to fully lock-free algorithms.

If you have questions or would like to discuss the project, feel free to reach out here on GitHub or on **Discord: `icantcode#7527`**.

---

## Overview

This repository provides reference implementations of several well-known synchronization strategies used in concurrent data structures.  
All implementations follow a consistent interface to make comparison, experimentation, and benchmarking easier.

---

## Implementations

### Synchronization Techniques

- **[Coarse-Grained Synchronization](/src/CoarseGrainedList.hpp)**  
  Uses a single global lock for the entire list. Simple but limited scalability.

- **[Fine-Grained Synchronization](/src/FineGrainedList.hpp)**  
  Uses per-node locking to increase concurrency while maintaining correctness.

- **[Optimistic Synchronization](/src/OptimisticList.hpp)**  
  Readers proceed without locking and validate state before committing changes.

- **[Lazy Synchronization](/src/LazyList.hpp)**  
  Builds on optimistic techniques with logical deletion and delayed physical removal.

- **[Lock-Free](/src/LockFreeList.hpp)**  
  A non-blocking implementation that avoids mutexes entirely, relying on atomic operations (CAS).

---

## Use Cases

These implementations are intended for:

- Students learning concurrent data structures  
- Developers comparing synchronization patterns  
- Research and experimentation with lock-free design  
- Benchmarking and performance analysis exercises  

If you would like examples, benchmarks, or extended documentation added, feel free to open an issue or submit a pull request.

---

## License

© [Luis Maya Aranda](https://github.com/v0-3). All rights reserved.  
Licensed under the **MIT License**.
