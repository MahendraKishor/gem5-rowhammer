"""
This script runs Google's `rowhammer-test` in full system mode on an x86 board
whose memory system models RowHammer with HammerSim. Ubuntu 22.04 is booted on
KVM cores; the simulation then switches to Timing cores for the hammering
iteration itself, which is the only part that needs a detailed memory system.

The disk image is the one built by `gem5-resources/src/rowhammer-fs`
(`./build-x86.sh 22.04`). Both it and the kernel can be overridden on the
command line.

Note on the kernel: `README-RH.md` documents the vanilla 5.4.49 kernel, but
that kernel hangs before printing anything when it is booted on gem5's KVM
cores (it does boot on the Atomic and Timing CPUs). The Ubuntu kernel from
gem5-resources boots under KVM, so it is the default here. It drives gem5's
IDE disk through libata, which is why the root device below is `/dev/sda`
and not the `/dev/hda` the vanilla kernel uses -- pass `--root-device` when
switching kernels.

Run with
1) For a baseline run with no TRR:
./build/X86/gem5.opt --outdir=m5out/rowhammer \
    store/scripts/x86-rowhammer-with-kvm.py --trr-variant 0

2) For a run with PARA:
./build/X86/gem5.opt --outdir=m5out/rh-para \
    store/scripts/x86-rowhammer-with-kvm.py --trr-variant 5 \
    --para-probability 0.01 --workload-timeout 60
"""
import argparse
import os
import sys

import m5.stats

_here = os.path.dirname(os.path.abspath(__file__))
_store_dir = os.path.dirname(_here)
_repo_root = os.path.dirname(_store_dir)
if _store_dir not in sys.path:
    sys.path.insert(0, _store_dir)

from components.RowhammerMemory import (
    TRR_VARIANTS,
    SingleChannelRowhammerDDR3_1600,
)
from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import (
    DiskImageResource,
    KernelResource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

# The disk image built by `gem5-resources/src/rowhammer-fs/build-x86.sh`. Its
# root file system is the second (GPT) partition.
DEFAULT_DISK_IMAGE = os.path.join(
    _repo_root,
    "gem5-resources",
    "src",
    "rowhammer-fs",
    "x86-disk-image-22-04",
    "x86-ubuntu",
)
DEFAULT_ROOT_PARTITION = "2"

# The kernel that boots this image on gem5's KVM cores, and the device name
# it gives the board's IDE disk. See the note at the top of this file.
DEFAULT_KERNEL = os.path.join(
    os.path.expanduser("~"),
    ".cache",
    "gem5",
    "x86-linux-kernel-5.4.0-105-generic",
)
DEFAULT_ROOT_DEVICE = "/dev/sda"

# `rowhammer-test` is cloned and built into the gem5 user's home directory by
# the disk image's post-installation script. `rowhammer_test` itself only
# hammers virtual addresses in a 1GiB anonymous mapping, so it does not need
# any privileges; `--use-sudo` is there for the variants of the benchmark that
# read /proc/self/pagemap. `12345` is the gem5 user's password in the image.
ROWHAMMER_TEST = "/home/gem5/rowhammer-test/rowhammer_test"
GEM5_USER_PASSWORD = "12345"


def get_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Google's rowhammer-test on a full system x86 board "
        "with HammerSim's RowHammer model enabled.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    parser.add_argument(
        "--kernel",
        type=str,
        default=DEFAULT_KERNEL,
        help="Path to the x86 Linux kernel to boot.",
    )
    parser.add_argument(
        "--disk-image",
        type=str,
        default=DEFAULT_DISK_IMAGE,
        help="Path to the rowhammer-fs Ubuntu disk image.",
    )
    parser.add_argument(
        "--root-partition",
        type=str,
        default=DEFAULT_ROOT_PARTITION,
        help="The disk image partition holding the root file system.",
    )
    parser.add_argument(
        "--root-device",
        type=str,
        default=DEFAULT_ROOT_DEVICE,
        help="The device name the kernel gives the board's IDE disk. Ubuntu "
        "kernels use libata (/dev/sda), the vanilla gem5 kernels use the "
        "legacy IDE driver (/dev/hda).",
    )
    parser.add_argument(
        "--switch-after-exits",
        type=int,
        default=3,
        help="Switch from the KVM cores to the Timing cores on this m5 exit "
        "event. There are three before the hammering starts: gem5_init.sh "
        "drops one once the kernel is up, after_boot.sh drops one before it "
        "reads the run script, and the run script itself drops one right "
        "before it starts rowhammer_test. The default of 3 therefore boots "
        "and starts the shell on KVM and hammers on the Timing cores.",
    )
    parser.add_argument(
        "--workload-timeout",
        type=int,
        default=600,
        help="Kill rowhammer_test after this many guest seconds so that "
        "after_boot.sh reaches its final m5 exit and the simulation ends. "
        "rowhammer_test never exits on its own (see the note at the top of "
        "this file). 0 disables the timeout.",
    )
    parser.add_argument(
        "--use-sudo",
        action="store_true",
        help="Run the workload under sudo. Not needed by rowhammer_test, and "
        "hashing the password in PAM costs a lot of simulated time on the "
        "Timing cores.",
    )
    parser.add_argument(
        "--num-cores",
        type=int,
        default=1,
        help="Number of cores to boot and to hammer with. Note that gem5's "
        "x86 KVM cores do not bring up secondary CPUs -- with more than one "
        "core the guest prints 'do_boot_cpu failed(-1) to wakeup CPU#1' and "
        "boots single-processor anyway, ten seconds later. rowhammer_test is "
        "single threaded.",
    )
    parser.add_argument(
        "--memory-size",
        type=str,
        default="2GiB",
        help="Size of the main memory. The x86 board maps everything below "
        "3GiB, so this cannot be larger than that.",
    )
    parser.add_argument(
        "--ranks-per-channel",
        type=int,
        default=1,
        help="Ranks per channel. Together with the memory size this decides "
        "the number of rows per bank, i.e. which rows of the device map are "
        "reachable.",
    )
    parser.add_argument(
        "--device-file",
        type=str,
        default=None,
        help="Path to the JSON device map of the modelled DIMM. Defaults to "
        "util/hammersim/prob-005.json.",
    )
    parser.add_argument(
        "--rowhammer-threshold",
        type=int,
        default=50000,
        help="Number of activates on an aggressor row which trigger "
        "RowHammer.",
    )
    parser.add_argument(
        "--trr-variant",
        type=int,
        default=0,
        choices=sorted(TRR_VARIANTS),
        help="The TRR variant to defend with: "
        + ", ".join(f"{key} = {value}" for key, value in TRR_VARIANTS.items()),
    )
    parser.add_argument(
        "--trr-threshold",
        type=int,
        default=32768,
        help="Activate count at which TRR refreshes a row. Unused when "
        "--trr-variant is 0 or 5.",
    )
    parser.add_argument(
        "--para-probability",
        type=float,
        default=0.01,
        help="Probability with which PARA refreshes a neighbour of an "
        "activated row. Only used when --trr-variant is 5.",
    )
    parser.add_argument(
        "--single-sided-prob",
        type=float,
        default=1e7,
        help="Number of single-sided RowHammer attacks needed before a "
        "bitflip is observed. rowhammer-test hammers single-sided first.",
    )
    parser.add_argument(
        "--double-sided-prob",
        type=float,
        default=1e5,
        help="The same, for double-sided attacks. Double-sided hammering is "
        "the most effective pattern, so this should be the smallest of the "
        "three probabilities.",
    )
    parser.add_argument(
        "--half-double-prob",
        type=float,
        default=1e10,
        help="The same, for half-double attacks.",
    )
    parser.add_argument(
        "--disable-memory-corruption",
        action="store_true",
        help="Count the bitflips in the stats, but do not actually flip bits "
        "in the backing store.",
    )
    parser.add_argument(
        "--rh-stat-dump",
        action="store_true",
        help="Dump a trace of the RowHammer triggers.",
    )
    parser.add_argument(
        "--rh-stat-file",
        type=str,
        default="rowhammer.trace",
        help="Where to write the RowHammer trace. A relative path lands in "
        "gem5's output directory.",
    )
    parser.add_argument(
        "--trr-stat-dump",
        action="store_true",
        help="Dump a trace of the TRR triggers.",
    )
    parser.add_argument(
        "--enable-ecc",
        action="store_true",
        help="Correct the RowHammer bitflips with a functional SECDED ECC.",
    )
    parser.add_argument(
        "--p-matrix",
        type=str,
        default=None,
        help="Path to the parity matrix used by the ECC. Required with "
        "--enable-ecc.",
    )
    parser.add_argument(
        "--ecc-algorithm",
        type=int,
        default=1,
        help="Index of the ECC algorithm to use (0 = none, 1 = SECDED). Only "
        "used with --enable-ecc.",
    )

    return parser.parse_args()


args = get_arguments()

# KVM is used to boot Ubuntu, so the host must be an x86 machine and gem5 must
# have been built with KVM support.
requires(isa_required=ISA.X86, kvm_required=True)

for description, path in (
    ("kernel", args.kernel),
    ("disk image", args.disk_image),
):
    if not os.path.isfile(path):
        raise FileNotFoundError(
            f"The {description} '{path}' does not exist. See README-RH.md for "
            f"how to build it."
        )

cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="32KiB", l1i_size="32KiB", l2_size="256KiB"
)

# The memory system doing the actual RowHammer modelling. `synthetic_traffic`
# is left False: this is a full system run, not a traffic generator.
memory_arguments = {
    "size": args.memory_size,
    "ranks_per_channel": args.ranks_per_channel,
    "rowhammer_threshold": args.rowhammer_threshold,
    "trr_variant": args.trr_variant,
    "trr_threshold": args.trr_threshold,
    "para_probability": args.para_probability,
    "single_sided_prob": int(args.single_sided_prob),
    "double_sided_prob": int(args.double_sided_prob),
    "half_double_prob": int(args.half_double_prob),
    "enable_memory_corruption": not args.disable_memory_corruption,
    "rh_stat_dump": args.rh_stat_dump,
    "rh_stat_file": args.rh_stat_file,
    "trr_stat_dump": args.trr_stat_dump,
    "enable_ecc": args.enable_ecc,
    "p_matrix": args.p_matrix,
    "ecc_algorithm": args.ecc_algorithm if args.enable_ecc else 0,
}

if args.device_file is not None:
    memory_arguments["device_file"] = args.device_file

memory = SingleChannelRowhammerDDR3_1600(**memory_arguments)

# Booting Ubuntu in a detailed CPU model takes hours, so the board boots on
# KVM cores and switches to Timing cores once `rowhammer_test` is about to
# start hammering.
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    num_cores=args.num_cores,
    isa=ISA.X86,
)

# gem5's KVM CPU uses the host's perf events to count guest instructions.
# On a host that restricts them (`kernel.perf_event_paranoid` > 0) the vCPU
# hangs before the guest prints anything, so perf is turned off here. Nothing
# in this script needs the instruction counts it provides.
for core in processor.get_cores():
    if core.is_kvm_core():
        core.core.usePerf = False

board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# This is run by `after_boot.sh` once the disk image has booted. The
# `gem5-bridge exit` in the middle is what switches the processor over to the
# Timing cores, so that the shell, sudo and `m5 readfile` still run on KVM.
workload = ROWHAMMER_TEST
if args.workload_timeout > 0:
    workload = f"timeout -s KILL {args.workload_timeout} {workload}"
if args.use_sudo:
    workload = f"echo {GEM5_USER_PASSWORD} | sudo -S {workload}"

command = [
    "echo rowhammer_test;",
    "gem5-bridge exit;",
    f"{workload};",
]

# The X86Board's default kernel arguments hardcode `/dev/hda` as the root
# device, which is only right for kernels using the legacy IDE driver, so the
# arguments are spelled out here instead. `mce=off` keeps the guest from
# polling machine check banks gem5 does not implement; see the note at the top
# of this file.
kernel_args = [
    "earlyprintk=ttyS0",
    "console=ttyS0",
    "lpj=7999923",
    "mce=off",
    f"root={args.root_device}{args.root_partition}",
    f"disk_device={args.root_device}",
]

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path=args.kernel),
    disk_image=DiskImageResource(
        local_path=args.disk_image,
        root_partition=args.root_partition,
    ),
    kernel_args=kernel_args,
    readfile_contents=" ".join(command),
)


def rowhammer_exit_event_handler():
    """Boots on the KVM cores, then hammers on the Timing cores.

    The disk image drops an `m5 exit` in `gem5_init.sh` once the kernel is up
    and another at the top of `after_boot.sh`, just before it runs the
    workload via `m5 readfile`; the run script itself drops a third one right
    before it starts hammering. `--switch-after-exits` decides which of them
    switches the processor; every exit event after that (the last one is the
    `gem5-bridge exit` at the end of `after_boot.sh`) ends the simulation.
    """
    exit_number = 0

    while True:
        exit_number += 1

        if exit_number < args.switch_after_exits:
            print(f"Exit event {exit_number}: booting on the KVM cores.")
            yield False
        elif exit_number == args.switch_after_exits:
            print(
                f"Exit event {exit_number}: switching to the Timing cores "
                "and resetting the stats before the hammering starts."
            )
            processor.switch()
            m5.stats.reset()
            yield False
        else:
            print(f"Exit event {exit_number}: the workload has finished.")
            yield True


simulator = Simulator(
    board=board,
    on_exit_event={ExitEvent.EXIT: rowhammer_exit_event_handler()},
)
simulator.run()

print(
    "Simulation finished after "
    f"{simulator.get_current_tick()} ticks: "
    f"{simulator.get_last_exit_event_cause()}"
)
