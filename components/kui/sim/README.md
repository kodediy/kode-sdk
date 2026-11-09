kui/sim
=======

The UI kit on a desktop, over SDL2. It shows one screen of a consumer in a window, or writes it to a PNG. The consumer in kodeOS is kgui (components/kgui/sim).

Build, from the kodeOS root:

    brew install sdl2 cmake pkgconf
    cmake -S components/kgui/sim -B build_kgui_sim
    cmake --build build_kgui_sim -j8

Run:

    build_kgui_sim/kgui_sim <screen> [--snapshot OUT.png] [--set SUBJECT=VALUE]... [--device] [--bezel]
    build_kgui_sim/kgui_sim face --snapshot face.png

Docs: https://docs.kode.diy/sdk/kui
