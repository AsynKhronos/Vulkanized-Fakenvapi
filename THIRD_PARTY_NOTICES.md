# Third-Party Notices

Vulkanized-Fakenvapi contains or consumes third-party material. The repository-level MIT license does **not** replace the license terms attached to those files.

| Component / material | Location | License / notice |
| --- | --- | --- |
| Original `fakenvapi` lineage | project history / derived code | MIT; upstream copyright retained in `LICENSE` |
| NVIDIA NVAPI open-source headers | `external/nvapi/` | MIT, as stated in the headers |
| AMD Anti-Lag 2 SDK headers | `external/ffx_antilag2_*.h` | MIT, as stated in the headers |
| LatencyFleX-derived header/code | `external/latencyflex.h` and derived implementation | Apache-2.0 notice in source; full text in `LICENSES/Apache-2.0.txt` |
| spdlog (including bundled fmt dependency notices) | `external/spdlog/` | MIT; see `external/spdlog/LICENSE` |
| Khronos Vulkan-Headers | `external/vulkan/` | Apache-2.0 / MIT as applicable; see `external/vulkan/LICENSE.md` |
| magic_enum | `external/magic_enum/` | MIT, as stated in source headers |
| Intel XeLL/XeSS SDK headers | `external/xell/` | Intel Simplified Software License; see `external/xell/LICENSE.txt` |
| Generated D3D12 header from Wine/VKD3D IDL | `external/d3d12.h` | LGPL-2.1-or-later, as stated in the file header; LGPL-2.1 text in `LICENSES/LGPL-2.1.txt` |
| Microsoft Detours | downloaded by `subprojects/detours.wrap` at pinned revision | upstream Detours license; source is built with MinGW during project build |

This file is a navigation aid, not a substitute for the full license text embedded in or shipped with each dependency.

## Trademarks and project names

NVIDIA, Reflex, NVAPI, AMD, Anti-Lag, Intel, XeLL, XeSS, Vulkan, OpenXR, Windows, DirectX, Wine, Proton and other names are trademarks or project identifiers of their respective owners. Their use here is descriptive and does not imply endorsement or affiliation.
