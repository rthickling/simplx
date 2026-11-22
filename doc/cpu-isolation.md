# Validate Simplx on isolated Linux CPUs

The suite starts two dedicated Simplx workers on separate physical cores and a third worker on a housekeeping core. It exercises the engine, Actor Events, timers, and the TCP connector. The TCP peer and the test watchdog run on housekeeping. The tests take Linux logical CPU IDs and can run with ordinary scheduling or with FIFO scheduling for the two dedicated workers.

## Build and run with `just`

Isolation recipes live in `isolation.just` (imported by the root justfile).
From the Simplx root, run `just test-isolation` to build the suite in `build/isolation` and run all four tests with ordinary scheduling. The launcher prefers two eligible isolated workers plus a housekeeping core. If there is no valid isolated layout, it tries three housekeeping cores. Check the reported `mode` and `cpus` to see what it selected.

```sh
just test-isolation
just test-isolation -- --cpus 12,14,10 --test events
just test-isolation -- --mode housekeeping
```

The CPU list is in worker A, worker B, housekeeping order. Use `--mode isolated` to require an isolated layout, `--pair same-die` or `--pair cross-die` to constrain the worker pair, and repeat `--test` to select multiple tests. An explicit CPU list must satisfy the selected mode. On an isolated boot, free the worker CPUs before running the suite. Use `just isolation-build` to build separately.

## Build with CMake

From the Simplx root, using the compiler described in [BUILDING.md](../BUILDING.md):

```sh
CXX=g++-16 cmake -S . -B build/isolation -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DSIMPLX_BUILD_TUTORIALS=OFF -DSIMPLX_BUILD_ISOLATION_TESTS=ON
cmake --build build/isolation --target simplx_isolation_tests
```

Use Linux with Python 3.10 or newer and `taskset` from util-linux. The isolation executables use the launcher below, which supplies the topology, scheduler settings, and watchdog. The regular `ctest` suite remains available in its own build tree.

## Choose CPUs and boot once with isolation

While booted normally, choose and record a layout. The order is worker A, worker B, housekeeping.

```sh
python3 tools/isolation.py plan
cpus=$(python3 tools/isolation.py plan --cpu-ids)
python3 tools/isolation.py plan --cpus "$cpus" --boot-args
```

The plan reads the Linux sibling masks and reports package, die, core, and NUMA node where the kernel exposes them. Core numbers can repeat across dies and packages. For a within-die run use `plan --pair same-die`. For a cross-die run use `plan --pair cross-die`, which also accepts a cross-socket pair. For each layout, retain the chosen CPU list and repeat the isolated boot when the worker pair changes. If the platform omits die information, use `--cpus A,B,HK` with IDs verified from the host topology. Same-die and cross-die results can differ because the worker-to-worker path crosses different cache or memory boundaries.

The suggested boot plan uses the lowest online logical ID per physical core as the candidate to survive `nosmt`. Confirm the actual IDs after reboot, especially on firmware with unusual CPU numbering. Keep CPU 0 for housekeeping. The test needs three distinct physical cores. Choose role IDs at or below 255, the range of Simplx's `CoreId`. A larger host can keep higher IDs in the system housekeeping set.

If you want a baseline with the same CPU IDs, run it on the normal boot before applying isolation:

```sh
python3 tools/isolation.py run --build build/isolation \
  --mode housekeeping --cpus "$cpus"
```

1. Reboot to the bootloader. In GRUB, highlight the usual Linux entry and press **e**. Append the printed fragment to the kernel line beginning with `linux` or `linuxefi`, before a `--` separator if present. Replace any existing settings for the same parameters for this boot. Press **Ctrl+X** or **F10** to boot the edited entry. For another bootloader, use its one-time kernel command-line editor or a separate test entry.
2. After boot, inspect `cat /proc/cmdline` and `lscpu -e=CPU,CORE,SOCKET,ONLINE`. Restore the recorded `cpus` shell variable, or use the planner to recover the active pair. The isolated launcher verifies that both worker CPUs are online, appear in `isolated` and `nohz_full`, and have no online SMT sibling. If a chosen CPU went offline, select IDs that survive `nosmt` and boot with the corrected worker list.
3. Check the launcher output for workqueue CPUs and effective IRQ targets on the two workers. If a movable IRQ or an unbound workqueue still targets a worker, correct its affinity using your host's administration procedures before measuring isolation. Save the original runtime settings if you change them. Managed or per-CPU IRQs can retain their affinity. Record their identities and check actual activity in the trace.

The generated fragment uses `nosmt`, `isolcpus=domain,managed_irq,…`, `nohz_full`, `rcu_nocbs` and `irqaffinity`. See the [Linux CPU isolation guide](https://docs.kernel.org/admin-guide/cpu-isolation.html) and [kernel command-line reference](https://docs.kernel.org/admin-guide/kernel-parameters.html) for the boot controls and their limits.

## Run on the isolated boot

Free the selected worker CPUs before starting an isolated test run. A live FIFO worker on either CPU can delay a test running at ordinary priority and invalidate the comparison.

```sh
# Restore cpus to the ordered A,B,HK values recorded before reboot.
python3 tools/isolation.py run --build build/isolation \
  --mode isolated --cpus "$cpus"

# Run just the engine's unprivileged FIFO-denial and recovery case.
python3 tools/isolation.py run --build build/isolation \
  --mode isolated --cpus "$cpus" --policy fifo-denied
```

The launcher expands the child process affinity to the three chosen CPUs and keeps the watchdog on housekeeping. With `--policy fifo-denied`, it lowers its own realtime-priority resource limit to zero before starting the test. Run that case as an ordinary user without `CAP_SYS_NICE`. To exercise affinity rejected by a cgroup as well, run the engine test inside a cpuset that permits the three roles and pass `--denied-cpu` with a fourth online CPU outside that cpuset. Use a kernel-enforced cpuset here because Simplx can widen inherited thread affinity when the cgroup permits it.

To run the positive FIFO case on a systemd host, grant a realtime-priority limit to a transient service running as your user:

```sh
sudo systemd-run --wait --pipe --collect --uid="$(id -u)" --gid="$(id -g)" \
  -p LimitRTPRIO=10 --working-directory="$PWD" \
  python3 tools/isolation.py run --build build/isolation \
  --mode isolated --cpus "$cpus" --policy fifo --priority 10
```

On another service manager, start the same Python command as a user with a matching `RLIMIT_RTPRIO` and a cgroup that permits FIFO. The runner exits nonzero if a test or the watchdog fails. Select one executable with `--test engine`, `--test events`, `--test timer`, or `--test tcp`. The default is all four.

- **Engine.** Checks exact sparse core discovery, reversed logical-to-node mapping, worker affinity and scheduler from callbacks, launcher restoration, cleanup and restart after failures. If this host has a representable offline CPU, it also attempts that placement.
- **Events.** Checks first delivery after idle, mixed payloads in both directions among three workers, an intentionally stalled receiver and drain, a credit-limited processing pipeline, return-to-sender, and shutdown with pending work followed by restart. Reports counts, backlog, worker progress gaps, and latency percentiles.
- **Timer.** Checks cross-core one-shot, repeating, and cancelled callbacks with a monotonic elapsed-time oracle, both idle and with concurrent Events. It reports lateness and allows 45 seconds for functional completion.
- **TCP.** Uses the Simplx connector and a loopback peer with fragmented input, a slow reader, exact ordered payload checks, and reconnect. Use a separate run with your actual network path if you need to qualify device IRQ and softirq placement.

A machine with three or four physical cores can run the isolated tests. It may have too few housekeeping cores for a separate three-worker housekeeping baseline on that boot. Use the normal boot for the baseline in that case.

## Compare the same workload and collect evidence

Use `--test events --messages 60000 --seconds 30 --timeout 90` with the same build and workload parameters for all runs. Compare ordinary housekeeping, ordinary isolated, and FIFO isolated. Run a second sample with `--contention-cpus` set to a housekeeping Linux CPU list. For example, `--contention-cpus "${cpus##*,}"` adds load to the housekeeping role's CPU. Keep the power profile consistent and record the Simplx revision, topology, boot settings, realtime runtime/period, workload count, and trace losses. Choose pass limits for maximum gaps and latency before collecting data. Run for many scheduler periods and use the measured gaps to identify any throttling or stalls.

Use a separate run for scheduler and IRQ tracing. Start tracing before the workload in a second terminal. Choose a capture length that covers startup and the whole run. With `trace-cmd` installed and a kernel that offers the `mono` trace clock:

```sh
sudo trace-cmd record -C mono -o isolation-trace.dat \
  -e sched:sched_switch -e irq:irq_handler_entry -e irq:softirq_entry sleep 120
sudo trace-cmd report -i isolation-trace.dat > isolation-trace.txt
```

Use the Events executable's `READY` records to correlate worker TIDs and monotonic timestamps with the trace. Simplx temporarily moves the launcher during actor construction, and a FIFO worker briefly uses ordinary scheduling during its startup handshake. After readiness, check that the test's housekeeping work stays off both isolated CPUs and that the intended worker TID accounts for userspace activity on each. Identify residual IRQ or kernel work, and record any lost trace events. Compare latency from the untraced run.

Use a separate build for event-page diagnostics. It counts the successful fresh-page and reuse branches from within each worker. The Events test forces a burst large enough to check the fresh-page path, drains it and checks reuse on subsequent bursts.

```sh
CXX=g++-16 cmake -S . -B build/isolation-pages -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
  -DSIMPLX_BUILD_TUTORIALS=OFF -DSIMPLX_BUILD_ISOLATION_TESTS=ON \
  -DSIMPLX_TEST_EVENT_PAGE_COUNTS=ON
cmake --build build/isolation-pages --target testisolatedevents.bin
python3 tools/isolation.py run --build build/isolation-pages \
  --mode isolated --cpus "$cpus" --test events
```

The CPU planner's synthetic topology tests can be run on any Linux test host with `python3 -B -m unittest discover -s test/isolated -p 'test_*.py'`.

## Return to a normal boot

Let the tests and tracing finish, then reboot and select the unchanged normal boot entry. A one-time GRUB edit is discarded at reboot. If you created a separate test entry, select the normal one. Verify the normal kernel parameters and online CPU list with `cat /proc/cmdline` and `lscpu -e=CPU,CORE,SOCKET,ONLINE`. Restore any persistent host IRQ or workqueue settings you changed for testing.
