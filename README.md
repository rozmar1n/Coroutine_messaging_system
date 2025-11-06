Coroutine Message Bus
=====================

This project implements a coroutine-friendly messaging bus (a set of bounded
channels similar to Go channels) that was originally posed as the first
assignment of the VK System Programming course. The full task statement can be
found in `1/task_eng.txt`.

## Project goals
- Provide a `corobus` object that hands out channels for message passing within a
  single-threaded coroutine scheduler.
- Implement blocking send/recv operations that suspend the current coroutine
  whenever the channel capacity is exceeded or the channel is empty, and resume
  them once the state changes.
- Offer non-blocking `try_*` helpers plus optional extensions: broadcast support
  (`NEED_BROADCAST`) and batch send/recv (`NEED_BATCH`).
- Keep the implementation free of global mutable state and memory leaks.

## Repository layout
- `1/` – sources, tests and the detailed specification for homework №1 (this
  project). Key files: `corobus.[ch]`, `libcoro.[ch]`, `test.c`.
- `utils/` – helper utilities such as `heap_help` for leak detection and
  intrusive list helpers in `rlist.h`.
- `allcups/` – Docker tooling for reproducing the VK All Cups autotest
  environment.

## Working with the project
1. Build and run the homework tests locally (within any non-Windows distro):
   ```bash
   cd 1
   make test
   ```
2. Optional: check for memory leaks with `utils/heap_help`.
3. To reproduce the remote CI locally, build the Docker image and run tests with
   the homework number (`HW`) matching the target directory:
   ```bash
   docker build . -t allcups -f allcups/DockAllcups.dockerfile
   docker run -v ./:/tmp/data --env IS_LOCAL=1 --env HW=1 allcups
   ```
4. The remote pipeline archives the solution (`zip -r solutions/1.zip 1/*`),
   mounts it into the image, and writes results to `allcups/output.json`. The
   helper scripts in `allcups/` mirror that flow if you need an end-to-end
   rehearsal.

