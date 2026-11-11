// SPDX-License-Identifier: GPL-3.0-or-later
// Inserted into services/fs_romfs.cpp by the port's source transform (runtime/nxrt_port/CMakeLists.txt), in place of the
// start of upstream scan(), which becomes scan_std() and stays unused. The folder walk goes through POSIX calls
// (src/port_fs.cpp) instead of std::filesystem's iterator, which cannot list the PS5 title's /app0 folders; the checks
// and messages are upstream's.
}  // namespace
}  // namespace nxrt::services::fs

extern "C" int pkp_port_read_dir(const char* path, void (*on_entry)(void* ctx, const char* name, int type, uint64_t size),
                                 void* ctx, int* err);

namespace nxrt::services::fs {
namespace {

bool scan(Node& dir, std::string& msg) {
    std::string host8;
    if (!to_utf8(dir.host, host8)) {
        msg = "cannot read folder " + shown(dir) + " (the host path does not convert to UTF-8)";
        return false;
    }
    struct Listing {
        std::vector<std::string> names;
        std::vector<int> types;
        std::vector<uint64_t> sizes;
    } l;
    int err = 0;
    const int r = pkp_port_read_dir(
        host8.c_str(),
        [](void* p, const char* name, int type, uint64_t size) {
            auto* ls = static_cast<Listing*>(p);
            ls->names.emplace_back(name);
            ls->types.push_back(type);
            ls->sizes.push_back(size);
        },
        &l, &err);
    if (r != 0) {
        msg = "cannot read folder " + shown(dir) + " (errno " + std::to_string(err) + ")";
        return false;
    }
    for (size_t i = 0; i < l.names.size(); i++) {
        const std::string& name = l.names[i];
        if (name.empty() || name.size() > kMaxName) {
            msg = "name cannot be stored in a RomFS (1 to 255 bytes of UTF-8): " + shown(dir) + "/" + name;
            return false;
        }
        Node k;
        k.name = name;
        k.path = dir.path + "/" + name;
        if (k.path.size() > kMaxPath) {
            msg = "path too long for a RomFS (0x300 bytes): " + k.path;
            return false;
        }
        std::wstring wn;
        if (!to_wide(name, wn)) {
            msg = "name is not valid UTF-8: " + k.path;
            return false;
        }
        k.host = dir.host + L"/" + wn;
        if (l.types[i] == 2) {
            k.is_dir = true;
            if (!scan(k, msg)) return false;
        } else if (l.types[i] == 1) {
            k.size = l.sizes[i];
        } else {
            msg = "not a file or folder: " + k.path;
            return false;
        }
        dir.kids.push_back(std::move(k));
    }
    // std::string's operator< compares unsigned char values, which is the byte order the RomFS hash tables expect.
    std::sort(dir.kids.begin(), dir.kids.end(), [](const Node& a, const Node& b) { return a.name < b.name; });
    return true;
}

