# Next Steps


- (Minor) We have a mechanism for incorporating external libraries into Eta at runtime.
   To support this, there is a generic `NativeObject` heap kind type. Some `stdlib` packages still define their own
   native object kinds; those should be replaced with `NativeObject`.

- Use of HiGHS - keep the built-in solvers and add a package for the HiGHS linear programming solver.

- Concurrency, GC & Performance Trade-offs
  Shared Global Heap Contention: Unlike Erlang's isolated per-actor heaps, Eta's shared 16-sharded global heap requires stop-the-world pauses (pause_for_gc()) during sweep phases, which can introduce latency spikes under high actor concurrency.
  Interpreter Loop Overhead: Uses traditional switch-case bytecode dispatch without computed gotos (direct threading) or JIT compilation for CPU-bound numerical loops.

Is this a problem, given they are part of the standard library? (As opposed to side-car libs like duckdb etc.)
- Heavy Build Dependencies: Strong link-time coupling to heavy C++ dependencies (Libtorch, Boost, Eigen, NNG) increases build times and distribution footprint.
