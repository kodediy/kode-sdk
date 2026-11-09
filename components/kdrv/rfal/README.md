# RFAL — the NFC library of STMicroelectronics, copied into the SDK

RFAL is the RF Abstraction Layer of STMicroelectronics (ST). RF means radio frequency. An abstraction layer is code that hides the details of a chip behind simple functions. RFAL is a library for the NFC chips of ST, and it comes with the driver of the ST25R3916 chip family. This folder of `kdrv` holds a copy of it. A copy of the code of another maker, kept in this repository, is **vendored** code. Read this file before you change this folder and before you distribute the SDK.

Everything under `rfal/` and `chip/` is **© STMicroelectronics**, copied without modification from **X-CUBE-NFC6**. The version is **RFAL 4.0.2** (`RFAL_VERSION` 0x040002 in `rfal/Inc/rfal_rf.h`). The chip driver is the one for the ST25R3916 family. This board carries an **ST25R3916B**, so the `CMakeLists.txt` of `kdrv` defines `ST25R3916B`.

X-CUBE-NFC6 is a software package that ST publishes.

Two files are not ST's:

- `port/rfal_platform.h` defines the `platformXxx()` macros. The code of ST calls these macros to reach the hardware. In this file, the macros are connected to kodeOS. The file also chooses which RFAL features the build compiles.
- `port/rfal_port.h` declares the functions that those macros call. `kdrv_st25r3916.c` defines them.

These two files are the port of RFAL to kodeOS.

## The licence

The code of ST is **not** under the GNU General Public License, the licence of the rest of the SDK. Its licence is SLA0051 of STMicroelectronics. The file `LICENSE.txt` in this folder holds the full text. It is the file that ST puts in the folder of RFAL 4.0.2, copied without modification.

The licence sets these two conditions, and others:

- You can use the code only with an NFC chip that ST makes. This board has an ST25R3916B, which is such a chip.
- You cannot distribute the code under an open source licence. So the code of ST stays under SLA0051, in this folder, apart from the code of Kode.

When you distribute the SDK, keep `LICENSE.txt` next to the code of ST.

The code of Kode has an additional permission that allows it to be linked with RFAL. The file `NOTICE.md` in the root of the repository holds this permission, and it says which licence applies to each part of the SDK.

## The code rules of kodeOS do not apply here

The code of ST is third-party code: code that another company wrote. Two sets of rules of kodeOS do not apply to it. The first set is the MISRA C rules, which are rules for safe C code. The second set is the Doxygen rules, which are rules for the comments of public names. The script `tools/check_layers.sh` skips the code of ST, in the same way as it skips `components/qpc/`, the vendored copy of QP/C. The compiler raises warnings for the sources of ST with this toolchain. The `CMakeLists.txt` of `kdrv` makes these warnings less strict for the sources of ST only.

Do not edit `rfal/` or `chip/`. A change that seems necessary belongs in the port, that is, in the files of `port/`. It can also go in a bug report to ST.
