## Network Monitor for XFCE

This project is my fork of [xfce-hkmon](https://lightful.github.io/xfce-hkmon/).

### Installation & Usage

1. Download [netmon.cpp](netmon.cpp) and compile it (you only need gcc or clang installed):
```bash
g++ -std=c++0x -O3 -lrt netmon.cpp -o netmon
```
2. Place the executable somewhere (e.g. /usr/local/bin)
3. Add a XFCE Generic Monitor Applet (comes with most distros) with these settings: no label, 1 second period, *Bitstream Vera Sans Mono* font (recommended) and the following command:
```
/usr/local/bin/netmon NET CPU TEMP IO RAM
```
_Or alternatively, you could try to run the given executable (x86_64)._