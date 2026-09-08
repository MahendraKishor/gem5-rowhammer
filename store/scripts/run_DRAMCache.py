"""
This script creates a simple system with an 8-core X86 O3 processor, a
direct-mapped DRAM cache memory system (a 256 MiB HBM2 cache in front of
16 GiB of DDR4 far memory) and a MESI two level cache hierarchy. The
system is then run with the BFS workload from the GAPBS benchmark suite.

Run with
./build/X86/gem5.opt --outdir=m5out/DRAMCache store/scripts/run_DRAMCache.py
"""
import os
import sys

_here = os.path.dirname(os.path.abspath(__file__))
_store_dir = os.path.dirname(_here)
if _store_dir not in sys.path:
    sys.path.insert(0, _store_dir)

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)
from components.DRAMCache import DRAMCache
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator

# Here we setup a MESI Two Level Cache Hierarchy.
cache_hierarchy = MESITwoLevelCacheHierarchy(
    l1d_size="16kB",
    l1d_assoc=8,
    l1i_size="16kB",
    l1i_assoc=8,
    l2_size="256kB",
    l2_assoc=16,
    num_l2_banks=1,
)

# Setup the system memory.
memory = DRAMCache(
    memory_size="16GiB",
    dram_cache_size="256MiB",
    block_size=64,
)

# Create a processor that runs the X86 ISA, has 8 cores and uses a simple
# O3-based CPU model.
processor = SimpleProcessor(cpu_type=CPUTypes.O3, isa=ISA.X86, num_cores=8)

# Create a simple board with the processor, memory and cache hierarchy.
board = SimpleBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)


# Set the workload to run the X86 GAPBS BFS benchmark.
board.set_workload(obtain_resource("x86-gapbs-bfs-run"))

# Create a simulator with the board and run it.
simulator = Simulator(board=board)
simulator.run()
