"""
Tutorial 1: your first gem5 simulation.

A minimal SE-mode system:
    1 TimingSimple core  ->  no caches  ->  single-channel DDR3

Run with
./build/X86/gem5.opt --outdir=m5out/tutorial store/tutorial/hello.py
"""

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator

# The four things every gem5 board needs.
cache_hierarchy = NoCache()
memory = SingleChannelDDR3_1600(size="512MiB")
processor = SimpleProcessor(cpu_type=CPUTypes.TIMING, isa=ISA.X86, num_cores=1)

board = SimpleBoard(
    clk_freq="1GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# The workload: a statically linked "hello world" shipped with gem5.
board.set_se_binary_workload(
    BinaryResource("tests/test-progs/hello/bin/x86/linux/hello")
)

simulator = Simulator(board=board)
simulator.run()

print(
    f"Exited @ tick {simulator.get_current_tick()} "
    f"because {simulator.get_last_exit_event_cause()}"
)
