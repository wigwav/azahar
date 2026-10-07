// Prints CityHash64 (Azahar's implementation) of byte ranges: cityhash_tool file offset size
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "common/cityhash.h"
int main(int argc, char** argv) {
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 1;
    long off = std::strtol(argv[2], nullptr, 0), size = std::strtol(argv[3], nullptr, 0);
    std::vector<char> buf(size);
    std::fseek(f, off, SEEK_SET);
    if (std::fread(buf.data(), 1, size, f) != (size_t)size) return 2;
    std::printf("%016llX\n", (unsigned long long)Common::CityHash64(buf.data(), size));
}
