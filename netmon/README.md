## Network Monitor for XFCE

This project is my fork of [xfce-hkmon](https://lightful.github.io/xfce-hkmon/).

### Installation & Usage

1. Download [netmon.cpp](netmon.cpp) and compile it (you only need gcc or clang installed):
```bash
g++ -std=c++0x -O3 -lrt netmon.cpp -o netmon
```
2. Place the executable somewhere (e.g. /usr/local/bin)
```
sudo mv netmon /usr/local/bin/
```
4. Add a XFCE Generic Monitor Applet (xfce4-genmon-plugin) with these settings: no label, 1 second period, *Bitstream Vera Sans Mono* font with size = 7 (recommended) and the following command:
```
/usr/local/bin/netmon NET
```