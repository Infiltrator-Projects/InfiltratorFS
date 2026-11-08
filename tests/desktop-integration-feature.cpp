// SPDX-License-Identifier: GPL-3.0-or-later
// Native Format 0.19 incompatible-compression feature contract.
// The checker reads a checkpoint header only; it never changes the image.
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=2) {std::cerr<<"Usage: check-feature IMAGE\n";return 2;}
    std::ifstream file(argv[1],std::ios::binary);
    if(!file) {std::cerr<<"Cannot read image\n";return 1;}
    file.seekg(148,std::ios::beg);
    std::array<std::uint8_t,8> bytes{};
    if(!file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()))) {
        std::cerr<<"Checkpoint header is truncated\n";return 1;
    }
    std::uint64_t flags=0;
    for(unsigned i=0;i<8;i++)flags|=std::uint64_t(bytes[i])<<(8*i);
    if(!(flags&0x1000ULL)) {
        std::cerr<<"Format 0.19 incompatible compression bit missing\n";return 1;
    }
    return 0;
}
