# Canoroid camera-port preview

This development build uses the Canoroid v0.1.0 photo workflow and DinoFrame Run. Match the camera model and the installed Canon firmware exactly.

| Camera | Canon firmware | Platform |
| --- | --- | --- |
| EOS 550D | 1.0.9 | 550D.109 |
| EOS 6D | 1.1.6 | 6D.116 |
| EOS 650D | 1.0.4 | 650D.104 |
| EOS 50D | 1.0.9 | 50D.109 |
| EOS 60D | 1.1.1 | 60D.111 |
| EOS 70D | 1.1.2 | 70D.112 |
| EOS 100D | 1.0.1 | 100D.101 |
| EOS 500D | 1.1.1 | 500D.111 |
| EOS 700D | 1.1.5 | 700D.115 |
| EOS 5D Mark II | 2.1.2 | 5D2.212 |

The first three targets are available as alpha.1, the seven additional targets as alpha.2. All ten await on-camera validation. In DinoFrame Run use INFO to pause on the 50D, 500D and 5D Mark II, and Av on the 100D. Use the physical direction buttons and SET, including on the 650D. FIRST-INSTALL includes the Magic Lantern installer for the model printed in the archive name. UPDATE is for an already bootable Magic Lantern card on the same camera and firmware. Back up the card before changing its contents.

Build from the firmware directory with the toolchain listed in the repository’s firmware/CANOROID.md, substituting the exact platform from the table.

```sh
make -C platform/550D.109 clean
make -C platform/550D.109 -j4 CANOROID=y canoroid-packages
```

Use a spare card for physical testing. Check normal Canon startup, opening and leaving Canoroid with Trash and MENU, empty View Photos, and two successive JPEG captures through Live View. Check that both original files and both processed copies open, then test RAW+JPEG, game input including duck/release, power cycling and card re-entry. Record the camera, firmware, Canon shooting settings, and the exact screen or per-image log for any failure.
