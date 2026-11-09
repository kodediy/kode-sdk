# Licences of kode-sdk

This file says which licence applies to each part of this repository. Read it before you distribute the SDK, or a firmware that you built with it.

The repository holds the work of these owners:

| Part | Owner | Licence |
|---|---|---|
| everything that this file does not name below | Kode | GPL-3.0-or-later, with the additional permission below |
| `components/qpc/` | Quantum Leaps, LLC | GPL-3.0-or-later, or a commercial licence of Quantum Leaps |
| `components/kdrv/rfal/rfal/` and `components/kdrv/rfal/chip/` | STMicroelectronics | SLA0051, which is not an open source licence |
| `components/kui/fonts/assets/` and `components/kui/fonts/` | the authors of the fonts | SIL Open Font License 1.1 |

## The code of Kode

Copyright (C) 2026 Kode.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

The file `LICENSE` holds the full text of the GNU General Public License, version 3.

The two files in `components/kdrv/rfal/port/` are code of Kode. They are the port of RFAL to this board.

### Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with the RF Abstraction Layer (RFAL) and the ST25R3916 driver of STMicroelectronics (or a modified version of those libraries), containing parts covered by the terms of the STMicroelectronics SLA0051 licence, the licensors of this Program grant you additional permission to convey the resulting work. Corresponding Source for a non-source form of such a combination shall include the source code for the parts of RFAL and of the ST25R3916 driver used as well as that of the covered work.

This permission applies only to the code of which Kode holds the copyright. Kode cannot give it for the code of another owner, and so it does not apply to QP/C.

If you change the code of Kode, you can remove this permission from your version. Section 7 of the GNU General Public License allows this.

## QP/C

`components/qpc/` holds a copy of QP/C, the framework that runs the Active Objects. Its owner is Quantum Leaps, LLC. Quantum Leaps offers it under the GNU General Public License, version 3 or later, or under a commercial licence. This repository uses it under the GNU General Public License.

`components/qpc/LICENSE` holds the notice of Quantum Leaps. Do not remove it.

## RFAL

`components/kdrv/rfal/rfal/` and `components/kdrv/rfal/chip/` hold a copy of RFAL, the NFC library of STMicroelectronics, and of the driver of the ST25R3916 chip. The owner is STMicroelectronics.

This code is **not** under the GNU General Public License. Its licence is SLA0051 of STMicroelectronics. `components/kdrv/rfal/LICENSE.txt` holds the full text. The licence sets these two conditions, and others:

- You can use the code only with an NFC chip that STMicroelectronics makes. The Kode Dot has an ST25R3916B, which is such a chip.
- You cannot distribute the code under an open source licence.

When you distribute this repository, keep `components/kdrv/rfal/LICENSE.txt` next to the code of STMicroelectronics, and do not put that code under another licence.

## The fonts

`components/kui/fonts/assets/` holds the fonts DM Sans and Outfit. Their licence is the SIL Open Font License, version 1.1. The files `OFL-DMSans.txt` and `OFL-Outfit.txt` in that folder hold the full text and the names of the authors. The files in `components/kui/fonts/` are these fonts in the form of C code, so the same licence applies to them.

## The firmware of the ESP32-C5

The folder `c5/` is a different repository. This file does not apply to it.

## The components that the build downloads

The ESP-IDF component manager downloads other components when you build: for example LVGL, esp-hosted and the driver of the display. They are not in this repository. Each one has its own licence, in its folder under `managed_components/`.
