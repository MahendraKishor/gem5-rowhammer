"""
A channeled DDR memory system with HammerSim's RowHammer model enabled.

`ChanneledMemory` builds its DRAM interfaces from a `DRAMInterface` class, so
the usual way of enabling RowHammer -- assigning to `memory._dram_class` --
mutates the interface *class* and therefore leaks into every other memory
system built from the same class in the same simulation. This component
instead passes the RowHammer parameters to each per-channel interface
*instance*, and validates them before gem5 fatals deep inside
`DRAMInterface::DRAMInterface()`.

The RowHammer parameters themselves are documented in
`src/mem/DRAMInterface.py`; the defaults below are the ones from that file.
"""

import os
from typing import (
    Optional,
    Type,
    Union,
)

from m5.objects import (
    DRAMInterface,
    MemCtrl,
)

from gem5.components.memory.abstract_memory_system import AbstractMemorySystem
from gem5.components.memory.dram_interfaces.ddr3 import DDR3_1600_8x8
from gem5.components.memory.dram_interfaces.ddr4 import DDR4_2400_8x8
from gem5.components.memory.memory import ChanneledMemory
from gem5.utils.override import overrides

# <repo root>/store/components/RowhammerMemory.py -> <repo root>
_REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)

# The device map shipped with the repository. `DRAMInterface.device_file`
# defaults to the same file, but resolved against the current working
# directory, which only works when gem5 is run from the repository root.
DEFAULT_DEVICE_FILE = os.path.join(
    _REPO_ROOT, "util", "hammersim", "prob-005.json"
)

# The TRR variants implemented by `dram_interface.cc`. Anything else makes
# gem5 fatal with "Unknown trr_variant detected!".
TRR_VARIANTS = {
    0: "no TRR",
    1: "Vendor A (table based, with a companion table)",
    2: "Vendor B (random sampler)",
    3: "Vendor C (2K activate counter)",
    4: "Vendor A (experimental, without the companion table)",
    5: "PARA (Y. Kim et al.)",
    6: "Vendor B (simplified sampler)",
}


class RowhammerMemory(ChanneledMemory):
    """A `ChanneledMemory` whose DRAM interfaces model RowHammer.

    Every parameter that is not documented here has the same meaning as the
    identically named parameter of `DRAMInterface`.
    """

    def __init__(
        self,
        dram_interface_class: Type[DRAMInterface],
        num_channels: Union[int, str],
        interleaving_size: Union[int, str],
        size: Optional[str] = None,
        addr_mapping: Optional[str] = None,
        device_file: str = DEFAULT_DEVICE_FILE,
        ranks_per_channel: Optional[int] = 1,
        rowhammer_threshold: int = 50000,
        trr_variant: int = 0,
        trr_threshold: int = 32768,
        counter_table_length: int = 16,
        companion_table_length: int = 8,
        companion_threshold: int = 1024,
        para_probability: float = 0.01,
        single_sided_prob: int = int(1e7),
        double_sided_prob: int = int(1e5),
        half_double_prob: int = int(1e9),
        enable_memory_corruption: bool = True,
        synthetic_traffic: bool = False,
        rh_stat_dump: bool = False,
        rh_stat_file: str = "rowhammer.trace",
        trr_stat_dump: bool = False,
        enable_ecc: bool = False,
        p_matrix: Optional[str] = None,
        ecc_algorithm: int = 0,
    ) -> None:
        """
        :param device_file: Path to the JSON device map holding the vulnerable
            columns of the modelled DIMM. Resolved against the repository root
            when relative.
        :param ranks_per_channel: Overrides the interface class' rank count.
            The number of rows per bank is derived from it, so it also decides
            which rows of the device map are reachable. `None` keeps the value
            of the interface class.
        :param single_sided_prob: Number of single-sided RowHammer attacks
            needed before a bitflip is observed. Larger means rarer.
        :param double_sided_prob: The same, for double-sided attacks. This is
            the most effective pattern, so it should be the smallest of the
            three probabilities.
        :param half_double_prob: The same, for half-double attacks.
        :param para_probability: The probability with which PARA refreshes a
            neighbour of an activated row. Only used when `trr_variant` is 5.
        :param enable_memory_corruption: Whether a triggered bitflip actually
            flips a bit in the backing store. When False the attack is only
            counted in the stats.
        :param synthetic_traffic: Set to True only for traffic generators,
            which would otherwise produce a bitflip on every activate once the
            RowHammer threshold is reached.
        :param rh_stat_file: Where the RowHammer trace is written. A relative
            path lands in gem5's output directory.
        :param p_matrix: Path to the parity matrix used by the functional ECC.
            Required when `enable_ecc` is set.
        """
        if trr_variant not in TRR_VARIANTS:
            raise ValueError(
                f"Unknown trr_variant {trr_variant}. Supported variants:\n"
                + "\n".join(
                    f"  {key}: {value}" for key, value in TRR_VARIANTS.items()
                )
            )

        if trr_variant == 5 and not 0.0 < para_probability <= 1.0:
            raise ValueError(
                "para_probability must be within (0, 1], got "
                f"{para_probability}"
            )

        if not os.path.isabs(device_file):
            device_file = os.path.join(_REPO_ROOT, device_file)
        if not os.path.isfile(device_file):
            raise FileNotFoundError(
                f"The device map '{device_file}' does not exist. Pass the "
                "path of a HammerSim device map (see util/hammersim) via "
                "`device_file`."
            )

        if enable_ecc:
            if p_matrix is None:
                raise ValueError(
                    "A `p_matrix` file is required when `enable_ecc` is set."
                )
            if not os.path.isabs(p_matrix):
                p_matrix = os.path.join(_REPO_ROOT, p_matrix)
            if not os.path.isfile(p_matrix):
                raise FileNotFoundError(
                    f"The pMatrix file '{p_matrix}' does not exist."
                )

        # These are handed to the DRAM interface constructor in
        # `_create_mem_interfaces_controller`, which `ChanneledMemory`'s
        # constructor calls. Attribute names starting with an underscore
        # bypass `SimObject.__setattr__`, so this may be set before
        # `super().__init__()` runs.
        self._rh_params = {
            "device_file": device_file,
            "rowhammer_threshold": rowhammer_threshold,
            "trr_variant": trr_variant,
            "trr_threshold": trr_threshold,
            "counter_table_length": counter_table_length,
            "companion_table_length": companion_table_length,
            "companion_threshold": companion_threshold,
            "para_probability": para_probability,
            "single_sided_prob": int(single_sided_prob),
            "double_sided_prob": int(double_sided_prob),
            "half_double_prob": int(half_double_prob),
            "enable_memory_corruption": enable_memory_corruption,
            "synthetic_traffic": synthetic_traffic,
            "rh_stat_dump": rh_stat_dump,
            "rh_stat_file": rh_stat_file,
            "trr_stat_dump": trr_stat_dump,
            "enable_ecc": enable_ecc,
            "ecc_algorithm": ecc_algorithm,
        }

        if p_matrix is not None:
            self._rh_params["p_matrix"] = p_matrix

        if ranks_per_channel is not None:
            self._rh_params["ranks_per_channel"] = ranks_per_channel

        super().__init__(
            dram_interface_class=dram_interface_class,
            num_channels=num_channels,
            interleaving_size=interleaving_size,
            size=size,
            addr_mapping=addr_mapping,
        )

    @overrides(ChanneledMemory)
    def _create_mem_interfaces_controller(self) -> None:
        self._dram = [
            self._dram_class(
                addr_mapping=self._addr_mapping, **self._rh_params
            )
            for _ in range(self._num_channels)
        ]

        self.mem_ctrl = [
            MemCtrl(dram=self._dram[i]) for i in range(self._num_channels)
        ]

    @overrides(ChanneledMemory)
    def _get_dram_size(self, num_channels: int, dram: DRAMInterface) -> int:
        # `ranks_per_channel` may have been overridden, in which case the
        # class' value would give the wrong default size.
        ranks_per_channel = self._rh_params.get(
            "ranks_per_channel", dram.ranks_per_channel.value
        )
        return num_channels * (
            dram.device_size.value
            * dram.devices_per_rank.value
            * ranks_per_channel
        )


def SingleChannelRowhammerDDR3_1600(
    size: Optional[str] = None, **kwargs
) -> AbstractMemorySystem:
    """A single channel DDR3_1600_8x8 DIMM modelling RowHammer.

    :param kwargs: The RowHammer parameters of `RowhammerMemory`.
    """
    return RowhammerMemory(DDR3_1600_8x8, 1, 64, size=size, **kwargs)


def SingleChannelRowhammerDDR4_2400(
    size: Optional[str] = None, **kwargs
) -> AbstractMemorySystem:
    """A single channel DDR4_2400_8x8 DIMM modelling RowHammer.

    :param kwargs: The RowHammer parameters of `RowhammerMemory`.
    """
    return RowhammerMemory(DDR4_2400_8x8, 1, 64, size=size, **kwargs)
