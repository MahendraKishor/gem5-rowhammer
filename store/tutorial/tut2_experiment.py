"""
Run the SAME workload on DIFFERENT hardware and compare.

Everything you would normally change by hand is a command-line flag here,
so one script can produce a whole design-space sweep.

Run with
./build/X86/gem5.opt --outdir=m5out/tut2-baseline store/tutorial/tut2_experiment.py
./build/X86/gem5.opt --outdir=m5out/tut2-bigl2 \
    store/tutorial/tut2_experiment.py --l2-size=1MB
./build/X86/gem5.opt --outdir=m5out/tut2-o3 \
    store/tutorial/tut2_experiment.py --cpu=o3
"""

import argparse

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory.single_channel import SingleChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator

parser = argparse.ArgumentParser()
parser.add_argument("--cpu", default="timing", choices=["timing", "o3"])
parser.add_argument("--l1d-size", default="32kB")
parser.add_argument("--l1i-size", default="32kB")
parser.add_argument("--l2-size", default="64kB")
parser.add_argument("--clk", default="3GHz")
args = parser.parse_args()

cpu_types = {"timing": CPUTypes.TIMING, "o3": CPUTypes.O3}

cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size=args.l1d_size,
    l1i_size=args.l1i_size,
    l2_size=args.l2_size,
)

board = SimpleBoard(
    clk_freq=args.clk,
    processor=SimpleProcessor(
        cpu_type=cpu_types[args.cpu], isa=ISA.X86, num_cores=1
    ),
    memory=SingleChannelDDR4_2400(size="512MiB"),
    cache_hierarchy=cache_hierarchy,
)

board.set_se_binary_workload(BinaryResource("store/tutorial/bench/mm"))

print(f"=== cpu={args.cpu} l1d={args.l1d_size} l2={args.l2_size} ===")
Simulator(board=board).run()
