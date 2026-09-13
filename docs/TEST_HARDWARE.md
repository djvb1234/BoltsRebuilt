# Development and validation hardware

This is the maintainer's current machine, recorded on **12 September 2026**. It is
the available test configuration, not a minimum specification or a recommended
purchase. No minimum CPU model, RAM capacity or VRAM capacity has been established.

| Component | Observed configuration |
| --- | --- |
| CPU | Intel Core Ultra 9 285K; 24 cores, 24 logical processors |
| GPU used by the public-preview launch checks | NVIDIA GeForce RTX 5070 Ti |
| GPU memory | 16 GB class; NVIDIA's local tool reports 16,303 MiB |
| NVIDIA driver | 616.56; Windows driver version 32.0.16.1656 |
| Installed memory | 32 GB, in two 16 GB modules |
| Operating system | Windows 11 IoT Enterprise LTSC, build 26100 |
| Display | MSI MPG 491C OLED |
| Desktop resolution and refresh | 5120 x 1440, 32:9, 144 Hz |

Hardware and Windows information were read from local system queries. The driver
and GPU memory were also checked with `nvidia-smi`. The two public-preview launch
logs selected the RTX 5070 Ti as their DXGI adapter. The machine also exposes Intel
integrated graphics, but those launch checks did not validate that adapter.

The display's refresh rate is not a measured game frame rate. Internal render
scale, guest pacing, local shaders and scene all affect performance. Screenshot
overlays and private development benchmarks are not performance results for a
fresh public build.

## What this establishes

The public source built and reached the title screen on this machine. The native
launch used a small local library of three vertex and three pixel shader stages,
with fallback for other draws. See the [validation record](VALIDATION.md).

AMD GPUs, Intel GPUs, other NVIDIA models, laptops, other CPU configurations and
other drivers have not been validated in the published record. **Untested means
unknown, not incompatible.** Windows editions other than the one above are also
not separate verified configurations. The preview targets Windows x64 generally;
the LTSC edition is not a stated requirement.

## Reporting another configuration

Successful reports are useful too. Include the release or commit, CPU, GPU and
VRAM, RAM, Windows build, graphics driver, display resolution/aspect, render scale,
and the scene or task you tested. State whether native shaders were prepared and
whether F5 changed the result. Report only what you exercised; reaching a menu
does not establish a complete playthrough.

Use the [first-playtest guide](FIRST_PLAYTEST.md) and
[playtest form](https://github.com/djvb1234/BoltsRebuilt/issues/new?template=playtest.yml).
Share relevant specifications rather than a full system report containing machine
names, serial numbers or personal paths.
