#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "analysis.h"
#include "codegen.h"

namespace fs = std::filesystem;

static uint32_t ParseAddr(std::string_view s) { return uint32_t(std::stoul(std::string(s), nullptr, 0)); }

static std::optional<int64_t> Num(const toml::table* t, const char* key) {
    return t ? (*t)[key].value<int64_t>() : std::nullopt;
}

// "ret" or an address, as a number or a string
static bool ParseExit(const toml::node* n, Hook::Exit& e) {
    if (!n) return true;
    e.set = true;
    if (auto i = n->value<int64_t>()) {
        e.to = uint32_t(*i);
        return true;
    }
    auto s = n->value<std::string>();
    if (!s) return false;
    if (*s == "ret") {
        e.ret = true;
        return true;
    }
    try {
        e.to = ParseAddr(*s);
    } catch (...) {
        return false;
    }
    return true;
}

// Keys beyond [xbe], [functions], [symbols], [data] and [layout]:
//   [xbe] data_ranges = [[lo, hi], ...]  bytes in code sections that are never code, hi exclusive
//   [xbe] trace = [address, ...]  log each call of these functions (caller, arguments) and its eax
//   [[hook]] at, call, after, on_true, on_false  call a host function before or after a guest instruction;
//     on_true/on_false make it return bool and name "ret" or an instruction of the same function to jump to
//   [[function]] at, end  an entry decoded no further than end (exclusive); it also stops other walks
//   [[switch]] at, targets  the targets of an indirect jmp, replacing table detection
//   [xbe] xdk = build  includes <--xdk folder>/<build>/layout.toml, and gives every name the host code binds
//     (see AddHostNames) that [symbols]/[data] do not list the address 0, so an image whose symbols were never
//     found still builds and traps where such a name is first needed
//   [abi] name = ["esi", "stack", ...]  where a [symbols] function takes each API argument when the image does not
//     use the convention the host declares: a register, or "stack" for the next stack slot, which the callee pops
static bool LoadConfig(const fs::path& path, Config& cfg, const fs::path& xdk_dir) {
    toml::table t;
    try {
        t = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "%s: %s\n", path.string().c_str(), std::string(e.description()).c_str());
        return false;
    }
    fs::path dir = path.parent_path();
    if (auto v = t["xbe"]["xdk"].value<int64_t>()) {
        if (xdk_dir.empty()) {
            std::fprintf(stderr, "%s: [xbe] xdk = %lld needs --xdk <the SDK's xdk folder>\n", path.string().c_str(), (long long)*v);
            return false;
        }
        cfg.xdk = uint32_t(*v);
        fs::path layout = xdk_dir / std::to_string(*v) / "layout.toml";
        if (!fs::exists(layout)) {
            std::fprintf(stderr, "%s: [xbe] xdk = %lld: %s does not exist\n", path.string().c_str(), (long long)*v,
                         layout.string().c_str());
            return false;
        }
        if (!LoadConfig(layout, cfg, xdk_dir)) return false;
    }
    auto includes = t["includes"].as_array();
    if (includes)
        for (auto& i : *includes)
            if (auto s = i.value<std::string>())
                if (!LoadConfig(dir / *s, cfg, xdk_dir)) return false;
    if (auto v = t["xbe"]["path"].value<std::string>()) cfg.xbe_path = *v;
    if (auto v = t["xbe"]["title"].value<int64_t>()) cfg.title = uint32_t(*v);
    if (auto v = t["xbe"]["entry"].value<int64_t>()) cfg.entry = uint32_t(*v);
    if (auto a = t["xbe"]["trace"].as_array())
        for (auto& i : *a) cfg.trace.insert(uint32_t(i.value_or(int64_t(0))));
    if (auto v = t["xbe"]["out"].value<std::string>()) cfg.out_dir = *v;
    if (auto v = t["xbe"]["partitions"].value<int64_t>()) cfg.partitions = uint32_t(*v);
    if (auto v = t["xbe"]["gap_fill"].value<bool>()) cfg.gap_fill = *v;
    if (auto v = t["xbe"]["pointer_seeds"].value<bool>()) cfg.pointer_seeds = *v;
    if (auto a = t["xbe"]["code_sections"].as_array())
        for (auto& i : *a)
            if (auto s = i.value<std::string>()) cfg.code_sections.push_back(*s);
    if (auto a = t["xbe"]["code_prefixes"].as_array())
        for (auto& i : *a)
            if (auto s = i.value<std::string>()) cfg.code_prefixes.push_back(*s);
    if (auto a = t["xbe"]["data_ranges"].as_array())
        for (auto& i : *a)
            if (auto r = i.as_array(); r && r->size() == 2)
                cfg.data_ranges.push_back({uint32_t(r->get(0)->value<int64_t>().value_or(0)),
                                           uint32_t(r->get(1)->value<int64_t>().value_or(0))});
    if (auto f = t["functions"].as_table())
        for (auto& [k, v] : *f) cfg.functions[ParseAddr(k.str())] = v.value<std::string>().value_or("");
    for (auto [key, out] : {std::pair{"symbols", &cfg.symbols}, std::pair{"data", &cfg.data}})
        if (auto f = t[key].as_table())
            for (auto& [k, v] : *f) (*out)[std::string(k.str())] = uint32_t(v.value<int64_t>().value_or(0));
    if (auto f = t["layout"].as_table())
        for (auto& [k, v] : *f)
            if (auto i = v.value<int64_t>()) cfg.layout[std::string(k.str())] = uint32_t(*i);
    if (auto f = t["abi"].as_table())
        for (auto& [k, v] : *f) {
            std::string name(k.str());
            auto fail = [&](const std::string& why) {
                std::fprintf(stderr, "%s: [abi] %s: %s\n", path.string().c_str(), name.c_str(), why.c_str());
                return false;
            };
            auto a = v.as_array();
            if (!a) return fail("not an array of argument locations");
            if (a->size() > 16) return fail("more than 16 arguments");
            std::vector<std::string> locs;
            for (auto& x : *a) {
                std::string s = x.value<std::string>().value_or("");
                static const std::set<std::string> known{"stack", "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"};
                if (!known.count(s)) return fail("\"" + s + "\" is not stack, eax, ebx, ecx, edx, esi, edi or ebp");
                if (s != "stack" && std::count(locs.begin(), locs.end(), s)) return fail(s + " holds two arguments");
                locs.push_back(s);
            }
            cfg.abi[name] = locs;
        }
    auto bad = [&](const char* key) {
        std::fprintf(stderr, "%s: bad [[%s]] entry\n", path.string().c_str(), key);
        return false;
    };
    if (auto a = t["hook"].as_array())
        for (auto& e : *a) {
            auto h = e.as_table();
            auto at = Num(h, "at");
            auto call = h ? (*h)["call"].value<std::string>() : std::nullopt;
            Hook k;
            if (!at || !call || !ParseExit(h->get("on_true"), k.on_true) || !ParseExit(h->get("on_false"), k.on_false))
                return bad("hook");
            k.at = uint32_t(*at);
            k.call = *call;
            k.after = (*h)["after"].value_or(false);
            cfg.hooks.emplace(k.at, k);
        }
    if (auto a = t["function"].as_array())
        for (auto& e : *a) {
            auto at = Num(e.as_table(), "at");
            if (!at) return bad("function");
            cfg.functions.emplace(uint32_t(*at), "");
            cfg.function_ends[uint32_t(*at)] = uint32_t(Num(e.as_table(), "end").value_or(0));
        }
    if (auto a = t["switch"].as_array())
        for (auto& e : *a) {
            auto h = e.as_table();
            auto at = Num(h, "at");
            auto ts = h ? (*h)["targets"].as_array() : nullptr;
            if (!at || !ts || ts->empty()) return bad("switch");
            auto& v = cfg.switches[uint32_t(*at)];
            v.clear();
            for (auto& x : *ts) {
                auto i = x.value<int64_t>();
                if (!i) return bad("switch");
                v.push_back(uint32_t(*i));
            }
        }
    for (auto& [name, va] : cfg.symbols)
        if (va) cfg.functions.emplace(va, name);
    return true;
}

// the code part of a line: a trailing // comment may name a macro without binding it
static std::string_view Code(std::string_view line) { return line.substr(0, line.find("//")); }

// A macro that binds the symbol one of its arguments names, and which argument that is: the XDK macros, such as
// GUEST(name) and XDK_DATA(name), plus the wrappers the sources define for themselves, such as
// CRT_MATH(name, fn), whose body is X86_FUNC(GUEST(name)).
struct Binding {
    std::string macro;
    size_t arg;
    bool is_data;
};

static std::string_view Trim(std::string_view s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

static bool IsName(std::string_view s) {
    if (s.empty() || !(std::isalpha(uint8_t(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(uint8_t(c)) || c == '_')) return false;  // TOKEN_##param is a fragment, not a name
    return true;
}

// The k-th (0 based) argument of the call whose "(" is at pos. A call that goes on below the line still yields
// the arguments written on it, which is all a name needs.
static std::string_view Arg(std::string_view code, size_t pos, size_t k) {
    if (pos >= code.size() || code[pos] != '(') return {};
    size_t start = pos + 1, depth = 1;
    for (size_t i = start; i < code.size(); i++) {
        char c = code[i];
        if (c == '(' || c == '{' || c == '[') {
            depth++;
        } else if (c == ')' || c == '}' || c == ']') {
            if (--depth == 0) return k ? std::string_view{} : Trim(code.substr(start, i - start));
        } else if (c == ',' && depth == 1) {
            if (!k) return Trim(code.substr(start, i - start));
            k--;
            start = i + 1;
        }
    }
    return k ? std::string_view{} : Trim(code.substr(start));
}

// f(binding, position of its "(") for each call of a binding macro in the code
template <typename F>
static void Calls(std::string_view code, const Binding& b, F f) {
    for (size_t at = code.find(b.macro); at != std::string_view::npos; at = code.find(b.macro, at + 1)) {
        if (at && (std::isalnum(uint8_t(code[at - 1])) || code[at - 1] == '_')) continue;  // CUSTOM_SYM( is not SYM(
        size_t p = at + b.macro.size();
        while (p < code.size() && (code[p] == ' ' || code[p] == '\t')) p++;
        if (p < code.size() && code[p] == '(') f(b, p);
    }
}

template <typename F>
static void Calls(std::string_view code, const std::vector<Binding>& bindings, F f) {
    for (auto& b : bindings) Calls(code, b, f);
}

// true when the code applies this binding macro to exactly this name
static bool Binds(std::string_view code, const Binding& b, std::string_view name) {
    bool binds = false;
    Calls(code, b, [&](const Binding& b, size_t pos) { binds |= Arg(code, pos, b.arg) == name; });
    return binds;
}

static bool Has(const std::vector<Binding>& bindings, std::string_view macro) {
    for (auto& b : bindings)
        if (b.macro == macro) return true;
    return false;
}

// The wrappers a file defines: when a definition's body applies a known binding macro to one of its parameters,
// each call binds whatever name that parameter takes there. One round adds CRT_MATH from GUEST, the next adds a
// wrapper of CRT_MATH.
static void AddForwards(std::string_view text, std::vector<Binding>& bindings) {
    size_t at = 0;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view head = Trim(text.substr(at, eol - at));
        at = eol + 1;
        if (head.substr(0, 7) != "#define") continue;
        std::string def{head};
        for (size_t end = eol; end < text.size();) {  // the definition goes on while the line ends with a backslash
            size_t b = def.find_last_not_of(" \r");
            if (b == std::string::npos || def[b] != '\\') break;
            size_t next = text.find('\n', end + 1);
            if (next == std::string_view::npos) next = text.size();
            def.resize(b);
            def += ' ';
            def += text.substr(end + 1, next - end - 1);
            end = next;
        }
        size_t name_b = def.find_first_not_of(" \t", 7);
        if (name_b == std::string::npos) continue;
        size_t name_e = name_b;
        while (name_e < def.size() && (std::isalnum(uint8_t(def[name_e])) || def[name_e] == '_')) name_e++;
        std::string macro = def.substr(name_b, name_e - name_b);
        if (name_e >= def.size() || def[name_e] != '(' || Has(bindings, macro)) continue;
        size_t close = name_e;  // the end of the parameter list
        for (size_t depth = 0; close < def.size(); close++) {
            if (def[close] == '(') depth++;
            else if (def[close] == ')' && !--depth) break;
        }
        if (close >= def.size()) continue;
        std::string_view body = std::string_view(def).substr(close + 1);
        std::vector<std::string_view> params;
        size_t start = name_e + 1;
        for (size_t i = start; i <= close; i++) {
            if (i == close || def[i] == ',') {
                params.push_back(Trim(std::string_view(def).substr(start, i - start)));
                start = i + 1;
            }
        }
        size_t found = bindings.size();
        for (size_t i = 0; i < params.size(); i++) {
            if (!IsName(params[i])) continue;
            for (size_t k = 0; k < found; k++)
                if (Binds(body, bindings[k], params[i])) bindings.push_back({macro, i, bindings[k].is_data});
        }
    }
}

// Every name the host code in this folder binds, as the runtime sees it: a name the host replaces must be a
// symbol (a trapping stub when the image has no such function) and a name the host reads must be data. Macro
// definitions name their parameters, not bindings, and are only read for the wrappers they declare.
static void ScanHostNames(const fs::path& dir, std::set<std::string>& functions, std::set<std::string>& data) {
    std::vector<Binding> bindings{{"XDK_DATA", 0, true},      {"SYM", 0, false},         {"GUEST_CALL", 0, false},
                                  {"GUEST_ORIG", 0, false},   {"GUEST", 0, false},       {"HLE_STDCALL", 0, false},
                                  {"HLE_FASTCALL", 0, false}, {"HLE_CDECL", 0, false},   {"HLE_WRAP", 0, false}};
    std::vector<std::string> sources;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (!it->is_regular_file() || (path.extension() != ".cpp" && path.extension() != ".h")) continue;
        std::ifstream f(path);
        if (!f) continue;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        sources.push_back(std::move(text));
    }
    for (int round = 0; round < 3; round++)  // a wrapper of a wrapper needs the round after the one that added it
        for (auto& text : sources) AddForwards(text, bindings);

    for (auto& text : sources) {
        bool definition = false;  // inside a macro definition, whose names are parameters, not bindings
        for (size_t at = 0; at < text.size();) {
            size_t eol = text.find('\n', at);
            if (eol == std::string::npos) eol = text.size();
            std::string_view line = std::string_view(text).substr(at, eol - at);
            at = eol + 1;
            size_t p = line.find_first_not_of(" \t");
            if (p != std::string_view::npos && line[p] == '#') definition = true;
            if (!definition && p != std::string_view::npos) {
                std::string_view code = Code(line);
                Calls(code, bindings, [&](const Binding& b, size_t pos) {
                    std::string_view name = Arg(code, pos, b.arg);
                    if (IsName(name)) (b.is_data ? data : functions).emplace(name);
                });
            }
            size_t last = line.find_last_not_of(" \t\r");  // the definition goes on while the line ends with a backslash
            definition = last != std::string_view::npos && line[last] == '\\';
        }
    }
}

// The names the selected build's signature set knows, which is wider than what today's host sources read.
static bool AddXdkNames(Config& cfg, const fs::path& xdk_dir) {
    if (!cfg.xdk || xdk_dir.empty()) return true;
    fs::path path = xdk_dir / std::to_string(cfg.xdk) / "signatures.toml";
    if (!fs::exists(path)) return true;
    toml::table t;
    try {
        t = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "%s: %s\n", path.string().c_str(), std::string(e.description()).c_str());
        return false;
    }
    // symbols and anchors are functions, data are the XDK globals the host reads
    for (auto [key, out] : {std::pair{"symbols", &cfg.symbols}, std::pair{"anchors", &cfg.symbols},
                            std::pair{"data", &cfg.data}})
        if (auto f = t[key].as_table())
            for (auto& [k, v] : *f) out->emplace(std::string(k.str()), 0);
    return true;
}

// The names this image's host code binds get a symbol with no address unless the config gives one: a port whose
// signature scan matched nothing (or only some functions, or a build with no signatures.toml at all) still gets a
// placeholder for each, so the runtime links and traps "not in this image" where the missing name is first needed
// instead of failing to compile inside the XDK headers. The signature set picks the classification first, then the
// host sources add what it does not name.
static bool AddHostNames(Config& cfg, const fs::path& xdk_dir, const fs::path& port_root) {
    if (!AddXdkNames(cfg, xdk_dir)) return false;
    std::set<std::string> functions, data;
    if (!xdk_dir.empty()) {
        fs::path runtime = xdk_dir.parent_path() / "runtime";
        if (fs::is_directory(runtime)) ScanHostNames(runtime, functions, data);
        else std::fprintf(stderr, "%s: no host sources, so names the runtime binds must be in the config\n",
                          runtime.string().c_str());
    }
    if (!port_root.empty()) ScanHostNames(port_root / "src", functions, data);
    for (auto& n : functions)
        if (!cfg.data.count(n)) cfg.symbols.emplace(n, 0);
    for (auto& n : data)
        if (!cfg.symbols.count(n)) cfg.data.emplace(n, 0);
    return true;
}

// overrides that analysis could not apply are config mistakes
static bool CheckOverrides(const Analysis& an) {
    bool ok = true;
    for (auto& [at, end] : an.cfg.function_ends)
        if (!an.functions.count(at) || (end && end <= at)) {
            std::fprintf(stderr, "[[function]] %08X: not decodable code or end not above it\n", at);
            ok = false;
        }
    for (auto& [at, targets] : an.cfg.switches) {
        const Insn* i = an.InsnAt(at);
        if (!i || i->flow != Flow::JmpIndirect) {
            std::fprintf(stderr, "[[switch]] %08X: not a decoded indirect jmp\n", at);
            ok = false;
        }
        for (uint32_t t : targets)
            if (!an.InsnAt(t)) {
                std::fprintf(stderr, "[[switch]] %08X: target %08X is not a decoded instruction\n", at, t);
                ok = false;
            }
    }
    return ok;
}

int main(int argc, char** argv) {
    std::vector<std::string> args;
    fs::path xdk_dir;
    bool usage = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--xdk")) args.push_back(argv[i]);
        else if (++i < argc) xdk_dir = argv[i];
        else usage = true;
    }
    if (args.empty() || usage) {
        std::fprintf(stderr, "usage: xbe_recomp <config.toml> [xbe override] [out override] [--xdk <SDK xdk folder>]\n");
        return 1;
    }
    Config cfg;
    fs::path cfg_path = args[0];
    // config/game.toml is the usual relative path, so the port folder is the working directory then
    fs::path port_root = cfg_path.parent_path().parent_path();
    if (port_root.empty()) port_root = ".";
    if (!LoadConfig(cfg_path, cfg, xdk_dir) || !AddHostNames(cfg, xdk_dir, port_root)) return 1;
    for (auto& [name, locs] : cfg.abi)
        if (!cfg.symbols.count(name)) {
            std::fprintf(stderr, "%s: [abi] %s is not a [symbols] name\n", args[0].c_str(), name.c_str());
            return 1;
        }
    fs::path root = cfg_path.parent_path().parent_path();  // the config sits in <game>/config
    fs::path xbe_path = args.size() > 1 ? fs::path(args[1]) : root / cfg.xbe_path;
    fs::path out_dir = args.size() > 2 ? fs::path(args[2]) : root / cfg.out_dir;

    std::ifstream f(xbe_path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", xbe_path.string().c_str());
        return 1;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    xbe::Image img;
    std::string err;
    if (!img.Parse(std::move(data), err)) {
        std::fprintf(stderr, "%s: %s\n", xbe_path.string().c_str(), err.c_str());
        return 1;
    }
    std::fprintf(stderr, "%s: title %08X entry %08X kernel thunk %08X\n", xbe_path.filename().string().c_str(),
                 img.title_id, img.entry, img.kernel_thunk);
    if ((cfg.title && cfg.title != img.title_id) || (cfg.entry && cfg.entry != img.entry)) {
        std::fprintf(stderr, "%s was written for title %08X entry %08X, not this image: delete it so build.bat writes a new one\n",
                     args[0].c_str(), cfg.title, cfg.entry);
        return 1;
    }

    Analysis an(img, cfg);
    an.Run();
    if (!CheckOverrides(an)) return 1;
    fs::create_directories(out_dir);
    CodeGen gen(an, out_dir);
    return gen.Run() ? 0 : 1;
}
