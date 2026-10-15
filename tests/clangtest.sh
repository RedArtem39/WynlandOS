#!/bin/sh
# WynlandOS - a C/C++ compiler from Ubuntu (clang, through leaf) building
# and running programs here (shtest runs it; needs the network).
cd "$(mktemp -d)"
echo wyn | ary -S leaf install clang make > /tmp/clang-install.log 2>&1 || { echo "leaf install failed"; tail -3 /tmp/clang-install.log; exit 1; }

cat > a.c <<'EOF'
#include <stdio.h>
int main(int argc, char **argv) { (void)argc; printf("c: argv[0] %s\n", argv[0]); return 0; }
EOF
clang -O2 -o a0 a.c && ln -sf "$PWD/a0" ./other && ./other

cat > h.cpp <<'EOF'
#include <iostream>
#include <vector>
int main() { std::vector<int> v{1, 2, 3}; int s = 0; for (int x : v) s += x; std::cout << "c++: sum " << s << std::endl; }
EOF
clang++ -O2 -o hpp h.cpp && ./hpp

printf 'all:\n\t@echo make: works\n' > Makefile
make
