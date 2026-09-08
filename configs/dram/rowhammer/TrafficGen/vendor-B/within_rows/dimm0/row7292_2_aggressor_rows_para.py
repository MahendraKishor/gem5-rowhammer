# Copyright (c) 2021-2025 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
Run with
./build/X86/gem5.opt --outdir=m5out/vendorB-row7292_para --debug-flags=RhBitflip \
	configs/dram/rowhammer/TrafficGen/vendor-B/within_rows/dimm0/row7292_2_aggressor_rows_para.py
"""

from m5.objects import *
import m5, os

# Need a couple of standard definitions for the rest of the script to work
# without any issues.
ROW_SIZE = 0x400
TARGET_BANK = 0x4
# Each tick is 1/4th of a pico-second
MIN_PERIOD = 1230000
MAX_PERIOD = 1520000

# From our test set, row 7292 is vulnerable on DIMM 0.
TARGET_ROW = 7292


# We create a simple memory interface class for evaluating rowhammer
class DRAM_TEST(DDR4_2400_8x8):
    """
    This class has 8 banks per rank.
    The rowbuffer size is 1KiB
    """

    # Use the correct device map
    device_file = os.path.join(
        os.getcwd(), "util/hammersim/row_experiment_vendor_b/dimm0.bank-4.json"
    )
    ranks_per_channel = 1
    # PARA (Y. Kim et al.). PARA has no sampler, so trr_threshold,
    # counter_table_length and companion_table_length are unused here.
    trr_variant = 5
    para_probability = 0.01
    trr_threshold = 16834
    rowhammer_threshold = 45000
    counter_table_length = 6
    # companion_table_length = 6
    rh_stat_dump = True
    # There is a very high probability for a bitflip however, bitflips become
    # highly likely as the rowhammer threshold is crossed.
    half_double_prob = 1e18
    # Modeling this based on DDR4 DIMMs
    double_sided_prob = 1e16
    # Single sided rowhammer is rare.
    single_sided_prob = 1e18
    synthetic_traffic = True


duration = int(1e11)

system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "4GHz"
system.clk_domain.voltage_domain = VoltageDomain()
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("1GB")]

system.generator0 = PyTrafficGen()
system.generator1 = PyTrafficGen()

system.mem_ctrl = MemCtrl()

system.mem_ctrl.dram = DRAM_TEST(range=system.mem_ranges[0])

system.membus = L2XBar()

system.membus.cpu_side_ports = system.generator0.port
system.membus.cpu_side_ports = system.generator1.port

# for testing the victim row

system.mem_ctrl.port = system.membus.mem_side_ports


# Our victim bank from the collected device map is in bank 4.
def get_data_chunk(row_number, width=8):
    return row_number * 128


# Addresses start from row 292 of bank 4.
def createLinearTraffic0(tgen):
    yield tgen.createLinear(
        duration,  # duration
        AddrRange(
            str(get_data_chunk(TARGET_ROW - 1) + (8 * TARGET_BANK)) + "kB"
        ).end,
        AddrRange(
            str(get_data_chunk(TARGET_ROW - 1) + (8 * TARGET_BANK) + 1) + "kB"
        ).end,
        64,  # block_size
        MIN_PERIOD,  # min_period
        MAX_PERIOD,  # max_period
        100,  # rd_perc
        0,
    )  # data_limit
    yield tgen.createExit(0)


def createLinearTraffic1(tgen):
    yield tgen.createLinear(
        duration,  # duration
        AddrRange(
            str(get_data_chunk(TARGET_ROW + 1) + (8 * TARGET_BANK)) + "kB"
        ).end,
        AddrRange(
            str(get_data_chunk(TARGET_ROW + 1) + (8 * TARGET_BANK) + 1) + "kB"
        ).end,
        64,  # block_size
        MIN_PERIOD,  # min_period
        MAX_PERIOD,  # max_period
        100,  # rd_perc
        0,
    )  # data_limit
    yield tgen.createExit(0)


root = Root(full_system=False, system=system)

m5.instantiate()

system.generator0.start(createLinearTraffic0(system.generator0))
system.generator1.start(createLinearTraffic1(system.generator1))

m5.simulate()
