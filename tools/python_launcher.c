/*
 * WynlandOS - /usr/bin/python3.14
 *
 * Ubuntu's python3.14 is linked at a fixed address (0x400000, not PIE),
 * and the low half of every address space here is the kernel's identity
 * map. So python3.14 is this: a position-independent main() handing over
 * to Ubuntu's libpython3.14.so -- the same interpreter, the same stdlib,
 * extension modules resolve its symbols from the library. Python finds
 * its prefix from where this executable is (/usr/bin -> /usr).
 *
 * Built by tools/stage_base.sh: gcc -O2 -fPIE -pie ... libpython3.14.so.1.0
 */
extern int Py_BytesMain(int argc, char **argv);

int main(int argc, char **argv)
{
    return Py_BytesMain(argc, argv);
}
