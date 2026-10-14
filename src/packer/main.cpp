// strojanka_pack <installer-stub> <staging-folder> <output>
// Build tool: appends every file in the staging folder to the installer stub.

#include <iostream>

#include "common/payload.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: strojanka_pack <installer-stub> <staging-folder> <output>\n";
        return 2;
    }
    std::string error;
    if (!strojanka::payload::pack(argv[1], argv[2], argv[3], &error)) {
        std::cerr << "packing failed: " << error << "\n";
        return 1;
    }
    std::vector<strojanka::payload::Entry> entries;
    if (!strojanka::payload::read_index(argv[3], entries, &error)) {
        std::cerr << "verification failed: " << error << "\n";
        return 1;
    }
    std::cout << "packed " << entries.size() << " files into " << argv[3] << "\n";
    return 0;
}
