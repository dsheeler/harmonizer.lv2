harmonizer.lv2 - Audio to Midi
==============================

harmonizer.lv2  uses the aubio toolkit for note onset and pitch detection
on audio input and outputs midi.

Install
-------
Compiling harmonizer requires the LV2 SDK, bash, gnu-make, and a c-compiler.

```bash
  git clone git://github.com/dsheeler/harmonizer.lv2.git
  cd harmonizer.lv2
  make
  sudo make install PREFIX=/usr
```

Note to packagers: The Makefile honors PREFIX and DESTDIR variables as well
 as CFLAGS, LDFLAGS and OPTIMIZATIONS (additions to CFLAGS).
