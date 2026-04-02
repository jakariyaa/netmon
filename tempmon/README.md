## CPU Temperature Monitor for XFCE

This program reads CPU temperature from the kernel's hwmon interface and prints it in a format suitable for the XFCE Generic Monitor (xfce4-genmon-plugin).

### Installation & Usage

1. Compile the program (you only need `gcc` or `clang` installed):
```bash
gcc -std=gnu11 -O3 -march=native -flto -pipe -s tempmon.c -o tempmon
```
2. Place the executable somewhere (e.g. `/usr/local/bin`):
```bash
sudo mv tempmon /usr/local/bin/
```
3. Add a XFCE Generic Monitor Applet (`xfce4-genmon-plugin`) with these settings:
- **Label:** none
- **Update period:** 1 or 2 second
- **Command:** 
```
/usr/local/bin/tempmon
```

If the program is unable to find a matching sensor on first run it will print an error to stderr. The program caches the discovered sensor path under your runtime directory to speed up subsequent runs.
