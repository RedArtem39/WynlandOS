#!/bin/sh
# Ubuntu's gcc on WynlandOS (boot.cfg "wlrun=sh+/usr/share/wynland/qs-smoke/gcc.sh"
# in a session of an administrator with the password "wyn"): installs gcc,
# g++ and make with leaf -- cc1 and the rest are linked at a fixed address
# (not PIE), which needed the higher-half kernel -- and builds small C and
# C++ programs. Its log on the serial console, every line "[gcc] ...".
exec 2>&1
say() { echo "[gcc] $*"; }
if ! command -v gcc >/dev/null; then
    say "installing"
    echo wyn | ary -S leaf update 2>&1 | tail -1 | sed 's/^/[gcc] leaf: /'
    echo wyn | ary -S leaf install gcc g++ make 2>&1 | tail -4 | sed 's/^/[gcc] leaf: /'
fi
say "version: $(gcc --version 2>&1 | head -1)"
say "cc1 type: $(file -L $(gcc -print-prog-name=cc1) 2>/dev/null | grep -o 'pie executable\|LSB executable' || readelf -h $(gcc -print-prog-name=cc1) 2>/dev/null | grep Type)"
mkdir -p /tmp/gc && cd /tmp/gc
cat > a.c <<'EOF'
#include <stdio.h>
#include <math.h>
int main(int c, char **v) { printf("c: %s %.3f\n", v[0], sqrt(2.0)); return 0; }
EOF
cat > b.cpp <<'EOF'
#include <iostream>
#include <vector>
#include <numeric>
#include <string>
int main() { std::vector<int> v{1, 2, 3}; std::cout << "c++: " << std::accumulate(v.begin(), v.end(), 0) << " " << std::to_string(42) << "\n"; }
EOF
cat > Makefile <<'EOF'
all: a b
a: a.c
	gcc -O2 -Wall -o a a.c -lm
b: b.cpp
	g++ -O2 -Wall -o b b.cpp
EOF
say "make: $(make 2>&1 | tail -3 | tr '\n' ' ')"
say "$(./a 2>&1)"
say "$(./b 2>&1)"
say "static: $(gcc -O2 -static -o s a.c -lm 2>&1 | tail -1; ./s 2>&1)"
say "done"
