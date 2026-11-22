#!/usr/bin/env python3
"""Plan a Linux CPU-isolation boot and run the pinned Simplx tests."""

import argparse
import contextlib
import dataclasses
import itertools
import json
import math
import os
from pathlib import Path
import resource
import subprocess
import sys


CPU_SYSFS = Path('/sys/devices/system/cpu')
MAX_CORE_ID = 255
TESTS = ('engine', 'events', 'timer', 'tcp')


def cpu_list(value):
    cpus = set()
    for part in value.split(',') if value.strip() else ():
        bounds = part.strip().split('-')
        if len(bounds) > 2 or not all(bound.isdecimal() for bound in bounds):
            raise ValueError(f'invalid Linux CPU list: {value!r}')
        first, last = int(bounds[0]), int(bounds[-1])
        if first > last:
            raise ValueError(f'descending Linux CPU range: {part!r}')
        cpus.update(range(first, last + 1))
    return cpus


def format_cpus(cpus):
    spans = []
    ordered = sorted(cpus)
    for _, group in itertools.groupby(enumerate(ordered), lambda item: item[1] - item[0]):
        span = [cpu for _, cpu in group]
        spans.append(str(span[0]) if len(span) == 1 else f'{span[0]}-{span[-1]}')
    return ','.join(spans)


def read_optional(path):
    try:
        return path.read_text().strip()
    except OSError:
        return None


def optional_id(path):
    value = read_optional(path)
    number = int(value) if value is not None else -1
    return number if number >= 0 else None


@dataclasses.dataclass(frozen=True)
class Cpu:
    package: int | None
    die: int | None
    core: int | None
    numa_node: int | None
    siblings: tuple[int, ...]


def read_topology(sysfs=CPU_SYSFS):
    topology = {}
    for cpu in sorted(cpu_list((sysfs / 'online').read_text())):
        directory = sysfs / f'cpu{cpu}'
        files = directory / 'topology'
        siblings = cpu_list((files / 'thread_siblings_list').read_text())
        if cpu not in siblings:
            raise ValueError(f'CPU {cpu} disappeared while reading topology; retry')
        node = next(iter(directory.glob('node[0-9]*')), None)
        topology[cpu] = Cpu(optional_id(files / 'physical_package_id'),
                            optional_id(files / 'die_id'), optional_id(files / 'core_id'),
                            int(node.name[4:]) if node else None, tuple(sorted(siblings)))
    return topology


def effective_cpuset():
    # An inherited task mask may exclude isolated CPUs even when the cgroup allows them.
    groups = read_optional(Path('/proc/self/cgroup')) or ''
    for line in groups.splitlines():
        if line.startswith('0::/'):
            path = Path('/sys/fs/cgroup') / line[4:] / 'cpuset.cpus.effective'
            value = read_optional(path)
            return cpu_list(value) if value else None
    return None


def primary_candidates(topology, cpus):
    return {min(cpus.intersection(topology[cpu].siblings)) for cpu in cpus}


def pair_matches(first, second, relation):
    if relation == 'any':
        return True
    if first.package is None or second.package is None:
        return False
    if first.package != second.package:
        return relation == 'cross-die'
    if first.die is None or second.die is None:
        return False
    return (first.die == second.die) == (relation == 'same-die')


def choose_cpus(topology, worker_pool, control_pool, relation, requested=None):
    if requested is not None:
        first, second, control = requested
        if not {first, second} <= worker_pool or control not in control_pool:
            raise ValueError('the requested CPUs must be online and eligible for their roles')
        if len({topology[cpu].siblings for cpu in requested}) != 3:
            raise ValueError('the three roles must use distinct physical cores (no SMT siblings)')
        if not pair_matches(topology[first], topology[second], relation):
            raise ValueError(f'the chosen worker CPUs do not satisfy --pair {relation}')
        return requested

    workers = primary_candidates(topology, {cpu for cpu in worker_pool if cpu <= MAX_CORE_ID})
    controllers = primary_candidates(topology, {cpu for cpu in control_pool if cpu <= MAX_CORE_ID})
    for second, first in itertools.combinations(sorted(workers, reverse=True), 2):
        if pair_matches(topology[first], topology[second], relation):
            used = {topology[first].siblings, topology[second].siblings}
            control = next((cpu for cpu in sorted(controllers, reverse=True)
                            if topology[cpu].siblings not in used), None)
            if control is not None:
                return first, second, control
    raise ValueError(f'no layout with three distinct eligible cores for --pair {relation}; '
                     'supply --cpus or use an available topology')


def boot_parameters(topology, roles):
    first, second, control = roles
    workers = {first, second}
    if 0 in workers:
        raise ValueError('keep CPU 0 on housekeeping for the isolated boot')
    primary = primary_candidates(topology, set(topology))
    if not {first, second, control} <= primary:
        raise ValueError('boot planning uses the lowest online logical CPU of each physical core; '
                         'one of the requested SMT threads may go offline with nosmt')
    housekeeping = primary - workers
    isolated_list = format_cpus(workers)
    return (f'nosmt isolcpus=domain,managed_irq,{isolated_list} nohz_full={isolated_list} '
            f'rcu_nocbs={isolated_list} irqaffinity={format_cpus(housekeeping)}')


def roles_list(value):
    try:
        fields = value.split(',')
        if len(fields) == 3 and all(field.isdecimal() for field in fields):
            roles = tuple(map(int, fields))
            if len(set(roles)) == 3 and max(roles) <= MAX_CORE_ID:
                return roles
    except ValueError:
        pass
    raise argparse.ArgumentTypeError('use three distinct Simplx CPU IDs (0–255) in A,B,HK order')


def run_layout(topology, isolated, nohz, available, mode, relation, requested):
    housekeeping = available - isolated
    if mode == 'isolated':
        online = set(topology)
        workers = {cpu for cpu in available & isolated & nohz
                   if len(set(topology[cpu].siblings) & online) == 1}
    else:
        workers = housekeeping
    if mode == 'isolated' and not workers:
        raise ValueError('no CPUs passed isolated/nohz_full and offline-SMT-sibling checks; run plan before reboot')
    return choose_cpus(topology, workers, housekeeping, relation, requested)


def select_run_layout(topology, isolated, nohz, available, mode, relation, requested):
    if mode != 'auto':
        return mode, run_layout(topology, isolated, nohz, available, mode, relation, requested)

    if requested is not None:
        isolated_workers = isolated.intersection(requested[:2])
        if len(isolated_workers) == 1:
            raise ValueError('--cpus must put both workers on isolated CPUs or both on housekeeping CPUs')
        mode = 'isolated' if isolated_workers else 'housekeeping'
        return mode, run_layout(topology, isolated, nohz, available, mode, relation, requested)

    try:
        return 'isolated', run_layout(topology, isolated, nohz, available, 'isolated', relation, None)
    except ValueError as isolated_error:
        try:
            return 'housekeeping', run_layout(topology, isolated, nohz, available, 'housekeeping', relation, None)
        except ValueError as housekeeping_error:
            raise ValueError(f'no usable layout (isolated: {isolated_error}; housekeeping: {housekeeping_error})') from None


def host_status(topology, roles, possible, isolated, nohz):
    selected = roles[:2]
    irq_targets = {}
    for path in Path('/proc/irq').glob('[0-9]*/effective_affinity_list'):
        affinity = cpu_list(read_optional(path) or '') & set(selected)
        if affinity:
            irq = path.parent.name
            irq_targets[irq] = {'cpus': sorted(affinity),
                                'actions': read_optional(Path('/sys/kernel/irq') / irq / 'actions')}
    workqueue = read_optional(Path('/sys/devices/virtual/workqueue/cpumask'))
    return {
        'cpus': dict(zip(('worker_a', 'worker_b', 'housekeeping'), roles)),
        'role_topology': {cpu: dataclasses.asdict(topology[cpu]) for cpu in roles},
        'topology': {cpu: dataclasses.asdict(info) for cpu, info in topology.items()},
        'online': format_cpus(topology),
        'possible': format_cpus(possible),
        'isolated': format_cpus(isolated),
        'nohz_full': format_cpus(nohz),
        'launcher_affinity': format_cpus(os.sched_getaffinity(0)),
        'cgroup_effective': format_cpus(effective_cpuset() or ()),
        'smt_active': read_optional(CPU_SYSFS / 'smt/active'),
        'kernel': os.uname().release,
        'kernel_parameters': read_optional(Path('/proc/cmdline')),
        'rt_runtime_us': read_optional(Path('/proc/sys/kernel/sched_rt_runtime_us')),
        'rt_period_us': read_optional(Path('/proc/sys/kernel/sched_rt_period_us')),
        'workqueue_mask': workqueue,
        'workers_in_workqueue_mask': [cpu for cpu in selected
                                     if int(workqueue.replace(',', ''), 16) & (1 << cpu)]
        if workqueue else None,
        'irq_targets_on_workers': irq_targets,
    }


def stop_load(process):
    process.terminate()
    with contextlib.suppress(subprocess.TimeoutExpired):
        process.wait(timeout=2)
        return
    process.kill()
    process.wait()


def execute(args, roles, housekeeping):
    cpus = ','.join(map(str, roles))
    tests = args.test or (('engine',) if args.policy == 'fifo-denied' else TESTS)
    if args.policy == 'fifo-denied' and tuple(tests) != ('engine',):
        raise ValueError('the fifo-denied run supports only --test engine')
    if args.contention_cpus - housekeeping:
        raise ValueError('--contention-cpus must be online housekeeping CPUs allowed by the cgroup')
    if args.contention_cpus & set(roles[:2]):
        raise ValueError('--contention-cpus cannot include either worker CPU')
    if not os.sched_get_priority_min(os.SCHED_FIFO) <= args.priority <= os.sched_get_priority_max(os.SCHED_FIFO):
        raise ValueError('--priority is outside this kernel’s FIFO range')

    binaries = [(test, args.build.resolve() / 'bin' / f'testisolated{test}.bin') for test in tests]
    for _, binary in binaries:
        if not binary.is_file():
            raise ValueError(f'{binary} is missing; build the simplx_isolation_tests target')
    os.sched_setscheduler(0, os.SCHED_OTHER, os.sched_param(0))
    os.sched_setaffinity(0, {roles[2]})
    if args.policy == 'fifo-denied':
        resource.setrlimit(resource.RLIMIT_RTPRIO, (0, 0))
    lifetime = args.timeout * len(tests) + 5
    load_code = ('from time import monotonic; '
                 'finish = monotonic() + ' + repr(lifetime) + '\nwhile monotonic() < finish: pass')
    with contextlib.ExitStack() as cleanup:
        for cpu in sorted(args.contention_cpus):
            load = subprocess.Popen([sys.executable, '-c', load_code])
            cleanup.callback(stop_load, load)
            os.sched_setaffinity(load.pid, {cpu})
        for name, binary in binaries:
            command = ['taskset', '-c', cpus, str(binary), '--cpus', cpus]
            if args.policy != 'ordinary':
                command += ['--fifo-priority', str(args.priority)]
            if args.policy == 'fifo-denied':
                command += ['--expect-fifo-denied']
            if name == 'engine' and args.denied_cpu is not None:
                command += ['--denied-cpu', str(args.denied_cpu)]
            if name == 'events':
                command += ['--messages', str(args.messages), '--seconds', str(args.seconds)]
            print(f'Running {name} on {cpus} with {args.policy} scheduling (timeout {args.timeout}s)', flush=True)
            subprocess.run(command, check=True, timeout=args.timeout)


def positive_number(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('use a finite positive value')
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--cpus', type=roles_list, metavar='A,B,HK', help='explicit logical CPU IDs')
    common.add_argument('--pair', choices=('any', 'same-die', 'cross-die'), default='any',
                        help='relationship between the two workers (a different package is also cross-die)')
    modes = parser.add_subparsers(dest='command', required=True)
    plan = modes.add_parser('plan', parents=[common], help='show topology and proposed boot arguments')
    plan_output = plan.add_mutually_exclusive_group()
    plan_output.add_argument('--boot-args', action='store_true', help='print only the kernel command-line fragment')
    plan_output.add_argument('--cpu-ids', action='store_true', help='print only the selected A,B,HK CPU IDs')
    run = modes.add_parser('run', parents=[common], help='validate placement and run the requested tests')
    run.add_argument('--build', type=Path, required=True, help='configured build directory')
    run.add_argument('--mode', choices=('auto', 'housekeeping', 'isolated'), default='auto',
                     help='prefer a valid isolated layout, then try housekeeping (default: auto)')
    run.add_argument('--policy', choices=('ordinary', 'fifo', 'fifo-denied'), default='ordinary')
    run.add_argument('--priority', type=int, default=10)
    run.add_argument('--test', action='append', choices=TESTS, help='repeat to select tests (default all)')
    run.add_argument('--seconds', type=positive_number, default=3.0, help='paced Event workload duration')
    run.add_argument('--messages', type=int, default=4096, help='paced Event workload message count')
    run.add_argument('--timeout', type=positive_number, default=60.0, help='watchdog seconds per executable')
    run.add_argument('--contention-cpus', type=cpu_list, default=set(), metavar='CPU-LIST',
                     help='start a bounded CPU load on each selected housekeeping CPU')
    run.add_argument('--denied-cpu', type=int, choices=range(MAX_CORE_ID + 1), metavar='CPU',
                     help='also test an online CPU excluded by a real cpuset')
    args = parser.parse_args()

    try:
        topology = read_topology()
        online = set(topology)
        possible = cpu_list((CPU_SYSFS / 'possible').read_text())
        isolated = cpu_list(read_optional(CPU_SYSFS / 'isolated') or '')
        nohz_text = read_optional(CPU_SYSFS / 'nohz_full')
        nohz = cpu_list(nohz_text) if nohz_text and nohz_text != '(null)' else set()
        effective = effective_cpuset()
        available = online if effective is None else online & effective
        if args.command == 'plan':
            plan_cpus = primary_candidates(topology, online) & available
            workers = {cpu for cpu in plan_cpus if 0 < cpu <= MAX_CORE_ID}
            if len(workers & isolated) >= 2 and args.cpus is None and args.pair == 'any':
                workers &= isolated
            if args.cpus is not None:
                roles = choose_cpus(topology, available, available, args.pair, args.cpus)
            else:
                roles = choose_cpus(topology, workers, plan_cpus - isolated, args.pair)
            parameters = boot_parameters(topology, roles)
            if args.boot_args:
                print(parameters)
            elif args.cpu_ids:
                print(','.join(map(str, roles)))
            else:
                status = host_status(topology, roles, possible, isolated, nohz)
                status['boot_parameters'] = parameters
                print(json.dumps(status, indent=2))
        else:
            if not 1 <= args.messages <= 250_000 or args.seconds > 3600:
                raise ValueError('use 1–250000 messages per direction and at most 3600 workload seconds')
            mode, roles = select_run_layout(topology, isolated, nohz, available, args.mode, args.pair, args.cpus)
            status = host_status(topology, roles, possible, isolated, nohz)
            status.update(mode=mode, policy=args.policy, priority=args.priority if args.policy != 'ordinary' else 0,
                          contention_cpus=sorted(args.contention_cpus))
            print(json.dumps(status, indent=2), flush=True)
            execute(args, roles, available - isolated)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{parser.prog}: {error}\n')


if __name__ == '__main__':
    main()
