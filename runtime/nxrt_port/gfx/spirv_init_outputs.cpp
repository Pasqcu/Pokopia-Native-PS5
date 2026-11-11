// SPDX-License-Identifier: GPL-3.0-or-later
// spirv_init_outputs: every Location output of a vertex, tessellation evaluation or geometry shader starts at (0,0,0,1), as on
// the console. The game's shaders write only some components of some outputs (the sky dome's and the clouds' vertex shaders
// store .w of one vec4 and .xyz of others) while the next stage reads all four. On the console (and in Ryujinx) an unwritten
// user output reads (0,0,0,1); in Vulkan it is undefined, and Mesa RADV hands garbage: a NaN that blacks out the HDR frame
// where it lands (black sky, black clouds) and that the bloom chain spreads into wide white halos. On the PS5 this showed as
// black clouds with a white outline and a white rim around the characters (frame trace: the scene target went non-finite at
// the sky / cloud draws, fragment shaders f937a09e and 86aa3b21).
//
// Port of spirv_init_outputs from featureJosh/pokopia-recomp, pull request 14 (issue 13: "SteamOS/RADV: black background and
// white halos (unwritten vertex outputs read as NaN)"), GPL-3.0, which the pinned upstream (drakolordx7/pokopia-recomp 2c29515)
// does not have. Self-contained here: the upstream function uses helpers private to nvn_spirv_patch.cpp. Called from
// create_module in nvn/nvn_pipeline.cpp (transform in runtime/nxrt_port/gfx.cmake); [nvn] init_outputs = false
// (NXRT_NVN_INIT_OUTPUTS=false) turns it off.
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace nxrt::nvn::be::gpu {
namespace {
enum : uint32_t {
    kOpLine = 8, kOpEntryPoint = 15, kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeVector = 23, kOpTypeArray = 28, kOpTypePointer = 32,
    kOpConstant = 43, kOpConstantComposite = 44, kOpFunction = 54, kOpVariable = 59, kOpStore = 62, kOpDecorate = 71,
    kOpEmitVertex = 218, kOpEmitStreamVertex = 220, kOpLabel = 248, kOpNoLine = 317,
    kDecBuiltIn = 11, kDecLocation = 30, kDecComponent = 31, kStorageOutput = 3,
    kModelVertex = 0, kModelTessEval = 2, kModelGeometry = 3,
};
struct Inst {
    size_t at;       // word index of the instruction
    uint32_t op, len;
};
}  // namespace

// Stores the console's start value (component 3 = 1, the others 0, counted from the Component decoration; 1.0f for floats, 1 for
// integers) to every Location output a Vertex / TessellationEvaluation / Geometry entry point lists, right after the leading
// OpVariables of its first block, and in a geometry shader again after every OpEmitVertex / OpEmitStreamVertex. Covers scalars and
// vectors of 32-bit float / int and Output arrays of them; built-ins are left alone. False (out untouched) for another stage, an
// unexpected module or nothing to initialise.
bool spirv_init_outputs(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    size_t entry_at = 0, first_function = 0;
    uint32_t entries = 0;
    struct Scalar {
        bool is_float;
        uint32_t width;
    };
    std::map<uint32_t, Scalar> scalars;                                   // OpTypeFloat / OpTypeInt id -> kind, width
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> vec_of;             // vector type -> (component type, count)
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> array_of;           // array type -> (element type, length constant id)
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> ptr_of;             // pointer type -> (storage class, pointee)
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> constants;          // (type, value) -> OpConstant id (one-word ones)
    std::map<uint32_t, uint32_t> const_value, location, component;
    std::map<uint32_t, bool> builtin;
    std::vector<std::pair<uint32_t, uint32_t>> output_vars;               // (variable id, pointer type) in declaration order
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == kOpEntryPoint && n.len >= 4) { if (!entries++) entry_at = n.at; }
        else if (n.op == kOpFunction) { if (!first_function) first_function = n.at; }
        else if (n.op == kOpTypeFloat && n.len >= 3) scalars[w[1]] = {true, w[2]};
        else if (n.op == kOpTypeInt && n.len == 4) scalars[w[1]] = {false, w[2]};
        else if (n.op == kOpTypeVector && n.len == 4) vec_of[w[1]] = {w[2], w[3]};
        else if (n.op == kOpTypeArray && n.len == 4) array_of[w[1]] = {w[2], w[3]};
        else if (n.op == kOpTypePointer && n.len == 4) ptr_of[w[1]] = {w[2], w[3]};
        else if (n.op == kOpConstant && n.len == 4) { const_value[w[2]] = w[3]; constants.emplace(std::make_pair(w[1], w[3]), w[2]); }
        else if (n.op == kOpDecorate && n.len >= 4 && w[2] == kDecLocation) location[w[1]] = w[3];
        else if (n.op == kOpDecorate && n.len >= 4 && w[2] == kDecComponent) component[w[1]] = w[3];
        else if (n.op == kOpDecorate && n.len >= 3 && w[2] == kDecBuiltIn) builtin[w[1]] = true;
        else if (n.op == kOpVariable && n.len == 4 && w[3] == kStorageOutput && !first_function) output_vars.emplace_back(w[2], w[1]);   // no initializer
    }
    if (entries != 1 || !first_function) return false;
    const uint32_t* ep = &in[entry_at];
    const uint32_t ep_len = in[entry_at] >> 16, model = ep[1], entry_func = ep[2];
    if (model != kModelVertex && model != kModelTessEval && model != kModelGeometry) return false;
    // OpEntryPoint model id name... interface ids: the name is a literal string. Only a variable the entry point lists is stored to
    // (before SPIR-V 1.4 the list must hold every Output variable the entry point uses).
    std::map<uint32_t, bool> in_interface;
    {
        const char* nm = reinterpret_cast<const char*>(&ep[3]);
        size_t chars = 0;
        while (chars < (ep_len - 3) * 4 && nm[chars]) ++chars;
        for (size_t k = 3 + chars / 4 + 1; k < ep_len; ++k) in_interface[ep[k]] = true;
    }

    // The value each output starts with: component 3 is 1, the others 0 (1.0f for a float, 1 for an integer), counted from the
    // variable's Component decoration. An Output array of such vectors gets it in every element. New constants get ids from the bound.
    std::vector<uint32_t> decls;
    auto scalar_const = [&](uint32_t type, uint32_t value) -> uint32_t {
        auto it = constants.find({type, value});
        if (it != constants.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | kOpConstant, type, id, value});
        constants[{type, value}] = id;
        return id;
    };
    std::map<std::vector<uint32_t>, uint32_t> composites;                 // (type, constituents...) -> OpConstantComposite id
    auto composite_const = [&](uint32_t type, const std::vector<uint32_t>& parts) -> uint32_t {
        std::vector<uint32_t> key{type};
        key.insert(key.end(), parts.begin(), parts.end());
        auto it = composites.find(key);
        if (it != composites.end()) return it->second;
        const uint32_t id = bound++;
        decls.push_back(((3u + static_cast<uint32_t>(parts.size())) << 16) | kOpConstantComposite);
        decls.push_back(type);
        decls.push_back(id);
        decls.insert(decls.end(), parts.begin(), parts.end());
        composites[key] = id;
        return id;
    };
    // the start value of a scalar or vector of 32-bit float / int whose first component sits in slot `first`; 0 for any other type
    auto start_value = [&](uint32_t type, uint32_t first) -> uint32_t {
        uint32_t comp = type, count = 1;
        const bool vector = vec_of.count(type) != 0;
        if (vector) {
            comp = vec_of[type].first;
            count = vec_of[type].second;
        }
        auto sc = scalars.find(comp);
        if (sc == scalars.end() || sc->second.width != 32 || count < 1 || count > 4) return 0;
        const uint32_t one = sc->second.is_float ? 0x3f800000u : 1u;
        std::vector<uint32_t> parts;
        for (uint32_t c = 0; c < count; ++c) parts.push_back(scalar_const(comp, first + c == 3 ? one : 0u));
        return vector ? composite_const(type, parts) : parts[0];
    };
    std::vector<uint32_t> stores;
    for (const auto& [var, ptr] : output_vars) {
        if (!location.count(var) || builtin.count(var) || !in_interface.count(var)) continue;
        auto p = ptr_of.find(ptr);
        if (p == ptr_of.end() || p->second.first != kStorageOutput) continue;
        const uint32_t type = p->second.second, first = component.count(var) ? component[var] : 0u;
        uint32_t value = 0;
        if (array_of.count(type)) {
            const auto [elem, len_id] = array_of[type];
            if (!const_value.count(len_id) || const_value[len_id] == 0 || const_value[len_id] > 64) continue;
            const uint32_t e = start_value(elem, first);
            if (!e) continue;
            value = composite_const(type, std::vector<uint32_t>(const_value[len_id], e));
        } else {
            value = start_value(type, first);
        }
        if (value) stores.insert(stores.end(), {(3u << 16) | kOpStore, var, value});
    }
    if (stores.empty()) return false;

    // the stores open the entry function, behind the leading OpVariable run of its first block
    size_t insert_at = 0;
    for (size_t k = 0; k < insts.size() && !insert_at; ++k) {
        if (insts[k].op != kOpFunction || in[insts[k].at + 2] != entry_func) continue;
        size_t j = k + 1;
        while (j < insts.size() && insts[j].op != kOpLabel && insts[j].op != kOpFunction) ++j;
        if (j >= insts.size() || insts[j].op != kOpLabel) return false;
        ++j;
        while (j < insts.size() && (insts[j].op == kOpVariable || insts[j].op == kOpLine || insts[j].op == kOpNoLine)) ++j;
        if (j < insts.size()) insert_at = insts[j].at;
    }
    if (!insert_at) return false;

    out.clear();
    out.reserve(in.size() + decls.size() + stores.size() * 4 + 8);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        if (n.at == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (n.at == insert_at) out.insert(out.end(), stores.begin(), stores.end());
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        // A geometry shader's outputs are undefined again after every emitted vertex (Vulkan). Maxwell keeps them instead, so a shader
        // that wrote an output once and emitted it twice would read (0,0,0,1) the second time; every GS this game ships rewrites each
        // output before each emit, or returns right after its only one (upstream's note).
        if (model == kModelGeometry && (n.op == kOpEmitVertex || n.op == kOpEmitStreamVertex)) out.insert(out.end(), stores.begin(), stores.end());
    }
    out[3] = bound;
    return true;
}

}  // namespace nxrt::nvn::be::gpu
