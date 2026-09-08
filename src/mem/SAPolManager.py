# Copyright (c) 2023 The Regents of the University of California
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

from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject
from m5.objects.AbstractMemory import AbstractMemory


class SAPolManager(AbstractMemory):
    type = "SAPolManager"
    cxx_header = "mem/sa_policy_manager.hh"
    cxx_class = "gem5::memory::SAPolManager"

    port = ResponsePort("This port responds to memory requests")
    loc_req_port = RequestPort(
        "Port to the local (DRAM cache) memory controller"
    )
    far_req_port = RequestPort(
        "Port to the far (backing store) memory controller"
    )

    loc_burst_size = Param.Unsigned(64, "Local memory burst size")
    far_burst_size = Param.Unsigned(64, "Far memory burst size")

    # loc_mem_ctrl = Param.MemCtrl("Local memory controller")
    # far_mem_ctrl = Param.MemCtrl("Far memory controller")

    loc_mem_policy = Param.Policy(
        "CascadeLakeNoPartWrs", "DRAM Cache access policy"
    )

    dram_cache_size = Param.MemorySize("128MiB", "Total DRAM cache capacity")
    block_size = Param.Unsigned(64, "Cache block (line) size in bytes")
    addr_size = Param.Unsigned(64, "Address width in bits")
    orb_max_size = Param.Unsigned(256, "Outstanding Requests Buffer size")
    crb_max_size = Param.Unsigned(32, "Conflicting Requests Buffer size")

    # -----------------------------------------------------------------------
    # Set-associativity parameter.
    # num_ways=1  → effectively direct-mapped (same as PolicyManager).
    # num_ways=4  → 4-way set associative.
    # Constraint: (dram_cache_size / block_size) must be divisible by num_ways.
    # -----------------------------------------------------------------------
    num_ways = Param.Unsigned(
        4, "Number of ways per set (N-way set associative)"
    )

    # -----------------------------------------------------------------------
    # Native DRAM Cache address mapping.
    #
    # The conventional mapping takes the set index from the lowest block
    # address bits, which scatters the blocks of a page over many sets. NDC
    # instead takes the index from the Physical Frame Number, so every block
    # of a page shares a set (i.e. a DRAM row), and demotes the intra-page
    # bits above the interleaving bits into the tag:
    #
    #   conventional: [ Tag | Index PA[11:6] | Offset PA[5:0] ]
    #   NDC:          [ TagHi | Index PFN | TagLo PA[11:8] | Index PA[7:6] |
    #                   Offset PA[5:0] ]
    #
    # With page_size=4KiB, block_size=64 and intlv_low_bits=2 this maps a 4KiB
    # page onto 4 sets of 16 blocks each, so a 16-way cache holds a whole page
    # without a conflict miss.
    # -----------------------------------------------------------------------
    ndc_addr_mapping = Param.Bool(
        False,
        "Use the Native DRAM Cache address mapping instead of the "
        "conventional low-order-bits set index",
    )

    page_size = Param.Unsigned(
        4096,
        "OS page size in bytes, used by the NDC address mapping to locate "
        "the boundary between the intra-page tag bits and the PFN index bits",
    )

    ndc_loc_addr_mapping = Param.Bool(
        False,
        "Address the DRAM cache device the way NDC does: the set "
        "index picks the row (via bank/rank/row) and the way picks "
        "the column within it, so the ways of a set are row-buffer hits. "
        "Requires the local interfaces mapped at 0 with range=dram_cache_size",
    )

    intlv_low_bits = Param.Unsigned(
        2,
        "Number of low address bits directly above the block offset that stay "
        "part of the set index because they select channel/bank group "
        "(PA[7:6] by default). Only used by the NDC address mapping",
    )

    always_hit = Param.Bool(False, "Force all accesses to be hits (debug)")
    always_dirty = Param.Bool(False, "Force all lines to be dirty (debug)")
    static_frontend_latency = Param.Latency("10ns", "Static frontend latency")
    static_backend_latency = Param.Latency("10ns", "Static backend latency")

    tRP = Param.Latency("Row precharge time")
    tRCD_RD = Param.Latency("RAS to Read CAS delay")
    tRL = Param.Latency("Read CAS latency")

    cache_warmup_ratio = Param.Float(
        0.7, "DRAM cache warmup ratio, after that it'll reset the stats"
    )

    bypass_dcache = Param.Bool(False, "if the DRAM cache needs to be bypassed")
