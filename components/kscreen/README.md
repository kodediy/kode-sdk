kscreen
=======

LVGL on the screen, driven from one Active Object of an app: the display, the flush to AO_Display, the touch feed and the render pass. examples/display_lvgl uses it. kodeOS's own interface (kgui) does not.

When the cell is critical on battery, before the board switches itself off, it covers the app with a notice: the app hands `POWER_CRITICAL_SIG` to `kscreen_critical()`. An app with a notice of its own handles the signal itself instead.

It also composes whole pictures with the P4's pixel accelerator: copy the frame on the screen, scale, dim and blend pictures, and show the result with an LVGL object (`kscreen_pic_*`, `kscreen_frame_create`).

Depends on: kode_sys, kui

Docs: https://docs.kode.diy/sdk/kscreen
