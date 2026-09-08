from typing import (
    List,
    Sequence,
    Tuple,
)

from m5.objects import (
    AddrRange,
    DDR4_2400_16x4,
    HBM_2000_4H_1x64,
    HBMCtrl,
    MemCtrl,
    Port,
    PolicyManager,
)
from m5.util.convert import toMemorySize as _to_bytes

from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.memory.abstract_memory_system import AbstractMemorySystem
from gem5.utils.override import overrides


class DRAMCache(AbstractMemorySystem):
    def __init__(
        self,
        memory_size: str = "3GB",
        dram_cache_size: str = "128MB",
        loc_mem_policy: str = "CascadeLakeNoPartWrs",
        orb_max_size: int = 128,
        crb_max_size: int = 32,
        always_hit: bool = False,
        always_dirty: bool = False,
        bypass_dcache: bool = False,
        cache_warmup_ratio: float = 0.7,
        block_size: int = 64,
        addr_size: int = 64,
        loc_burst_size: int = 64,
        far_burst_size: int = 64,
        static_frontend_latency: str = "10ns",
        static_backend_latency: str = "10ns",
        tRP: str = "14ns",
        tRCD_RD: str = "12ns",
        tRL: str = "18ns",
        hbm_read_buf_size: int = 64,
        hbm_write_buf_size: int = 64,
        ddr4_read_buf_size: int = 64,
        ddr4_write_buf_size: int = 64,
    ) -> None:
        super().__init__()

        self._size = _to_bytes(memory_size)
        self._mem_range = AddrRange(memory_size)

        self.policy_manager = PolicyManager(
            range=self._mem_range,
            dram_cache_size=dram_cache_size,
            loc_mem_policy=loc_mem_policy,
            orb_max_size=orb_max_size,
            crb_max_size=crb_max_size,
            always_hit=always_hit,
            always_dirty=always_dirty,
            bypass_dcache=bypass_dcache,
            cache_warmup_ratio=cache_warmup_ratio,
            block_size=block_size,
            addr_size=addr_size,
            loc_burst_size=loc_burst_size,
            far_burst_size=far_burst_size,
            static_frontend_latency=static_frontend_latency,
            static_backend_latency=static_backend_latency,
            tRP=tRP,
            tRCD_RD=tRCD_RD,
            tRL=tRL,
        )

        # ── HBM2 local memory (DRAM cache) ─────────────────────────────────
        self.loc_mem_ctrl = HBMCtrl()
        self.loc_mem_ctrl.dram = HBM_2000_4H_1x64(
            range=self._mem_range,
            in_addr_map=False,
            null=True,
            kvm_map=False,
        )
        self.loc_mem_ctrl.dram_2 = HBM_2000_4H_1x64(
            range=self._mem_range,
            in_addr_map=False,
            null=True,
            kvm_map=False,
        )
        self.loc_mem_ctrl.dram.read_buffer_size = hbm_read_buf_size
        self.loc_mem_ctrl.dram.write_buffer_size = hbm_write_buf_size
        self.loc_mem_ctrl.dram_2.read_buffer_size = hbm_read_buf_size
        self.loc_mem_ctrl.dram_2.write_buffer_size = hbm_write_buf_size

        # ── DDR4 far memory (backing store) ────────────────────────────────
        self.far_mem_ctrl = MemCtrl()
        self.far_mem_ctrl.dram = DDR4_2400_16x4(
            range=self._mem_range,
            in_addr_map=False,
            null=True,
            kvm_map=False,
        )
        self.far_mem_ctrl.dram.read_buffer_size = ddr4_read_buf_size
        self.far_mem_ctrl.dram.write_buffer_size = ddr4_write_buf_size
        self.loc_mem_ctrl.port = self.policy_manager.loc_req_port
        self.far_mem_ctrl.port = self.policy_manager.far_req_port

    @overrides(AbstractMemorySystem)
    def incorporate_memory(self, board: AbstractBoard) -> None:
        pass

    @overrides(AbstractMemorySystem)
    def get_mem_ports(self) -> Sequence[Tuple[AddrRange, Port]]:
        return [(self._mem_range, self.policy_manager.port)]

    @overrides(AbstractMemorySystem)
    def get_memory_controllers(self) -> List[MemCtrl]:
        return [self.far_mem_ctrl]

    # Not part of AbstractMemorySystem's interface; provided as a convenience
    # for scripts that need the memory interfaces of this memory system.
    def get_mem_interfaces(self) -> List:
        return [self.policy_manager]

    @overrides(AbstractMemorySystem)
    def get_size(self) -> int:
        return self._size

    @overrides(AbstractMemorySystem)
    def set_memory_range(self, ranges: List[AddrRange]) -> None:
        if len(ranges) != 1 or ranges[0].size() != self._size:
            raise Exception(
                "DRAMCache requires a single contiguous range matching "
                f"memory_size.\n"
                f"Range size given : {ranges[0].size()}\n"
                f"Expected         : {self._size}"
            )
        self._mem_range = ranges[0]
        self.policy_manager.range = self._mem_range
        self.far_mem_ctrl.dram.range = self._mem_range
        self.loc_mem_ctrl.dram.range = self._mem_range
        self.loc_mem_ctrl.dram_2.range = self._mem_range

    def get_uninterleaved_range(self) -> List[AddrRange]:
        return [self._mem_range]
