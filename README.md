# STM32N6 AB-Encoder Fault Detection (LSTM)

Bachelor thesis firmware: capture quadrature encoder signals on an STM32N657X0
and classify faults with a compact LSTM running on the NPU (X-CUBE-AI).

Contamination and electrical noise on AB encoders produce missing pulses,
illegal transitions and index (Z) errors. This project timestamps A/B/Z edges
on the MCU, builds short feature windows and runs the detector on-device.
Fault cases were also generated with a function generator for repeatable tests.

Training and offline analysis (Python / MATLAB) were done off-target and are
not in this repository. 

## Scope

- STM32N657X0 application + FSBL
- Quadrature AB encoder input (A/B/Z)
- LSTM compiled with X-CUBE-AI for the NPU
- Network blob: `network_atonbuf.xSPI2.raw`
- CubeMX project: `AB_LSTM.ioc`

## Layout

| Path | Contents |
| --- | --- |
| `Appli/` | Application, HAL, X-CUBE-AI runtime |
| `FSBL/` | First-stage bootloader |
| `Drivers/` | STM32N6 HAL / CMSIS |
| `AB_LSTM.ioc` | CubeMX configuration |
| `network_atonbuf.xSPI2.raw` | Compiled NPU network buffer |

## Build

1. STM32CubeIDE with STM32N6 support and X-CUBE-AI.
2. Open `AB_LSTM.ioc` and the `Appli` / `FSBL` projects.
3. Build FSBL and Appli, then program the N6 board (FSBL first).

## License

No separate license is published for the thesis application code.
ST HAL, X-CUBE-AI and other vendor components keep their own licenses.
