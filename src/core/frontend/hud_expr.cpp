// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include "core/core.h"
#include "core/frontend/hud.h"
#include "core/frontend/hud_expr.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"

namespace ScreenRegions {

struct ExprNode {
    enum class Kind { Num, Str, Var, Unary, Binary, Ternary, Call } kind = Kind::Num;
    s64 num = 0;
    std::string text; ///< string literal, variable name, function name or operator
    std::vector<std::shared_ptr<ExprNode>> args;
};

namespace {

using NodePtr = std::shared_ptr<ExprNode>;

// ---------------------------------------------------------------------------------------------
// Parser

class Parser {
public:
    explicit Parser(const std::string& s) : src(s) {}

    NodePtr ParseAll() {
        NodePtr n = Ternary();
        Skip();
        if (pos != src.size()) {
            throw std::runtime_error("unexpected '" + src.substr(pos, 8) + "'");
        }
        return n;
    }

private:
    const std::string& src;
    size_t pos = 0;

    void Skip() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) {
            ++pos;
        }
    }
    bool Accept(const char* op) {
        Skip();
        const size_t n = std::char_traits<char>::length(op);
        if (src.compare(pos, n, op) == 0) {
            // don't split "<=" when asking for "<", etc.
            if (n == 1 && pos + 1 < src.size()) {
                const char c = src[pos], d = src[pos + 1];
                if ((c == '<' || c == '>' || c == '=' || c == '!') && d == '=') {
                    return false;
                }
                if ((c == '&' && d == '&') || (c == '|' && d == '|')) {
                    return false;
                }
            }
            pos += n;
            return true;
        }
        return false;
    }
    void Expect(const char* op) {
        if (!Accept(op)) {
            throw std::runtime_error(std::string("expected '") + op + "'");
        }
    }
    static NodePtr Bin(const std::string& op, NodePtr a, NodePtr b) {
        auto n = std::make_shared<ExprNode>();
        n->kind = ExprNode::Kind::Binary;
        n->text = op;
        n->args = {std::move(a), std::move(b)};
        return n;
    }

    NodePtr Ternary() {
        NodePtr c = Or();
        if (Accept("?")) {
            NodePtr a = Ternary();
            Expect(":");
            NodePtr b = Ternary();
            auto n = std::make_shared<ExprNode>();
            n->kind = ExprNode::Kind::Ternary;
            n->args = {c, a, b};
            return n;
        }
        return c;
    }
    NodePtr Or() {
        NodePtr a = And();
        while (Accept("||")) {
            a = Bin("||", a, And());
        }
        return a;
    }
    NodePtr And() {
        NodePtr a = Equality();
        while (Accept("&&")) {
            a = Bin("&&", a, Equality());
        }
        return a;
    }
    NodePtr Equality() {
        NodePtr a = Relational();
        for (;;) {
            if (Accept("==")) {
                a = Bin("==", a, Relational());
            } else if (Accept("!=")) {
                a = Bin("!=", a, Relational());
            } else {
                return a;
            }
        }
    }
    NodePtr Relational() {
        NodePtr a = Bitwise();
        for (;;) {
            if (Accept("<=")) {
                a = Bin("<=", a, Bitwise());
            } else if (Accept(">=")) {
                a = Bin(">=", a, Bitwise());
            } else if (Accept("<")) {
                a = Bin("<", a, Bitwise());
            } else if (Accept(">")) {
                a = Bin(">", a, Bitwise());
            } else {
                return a;
            }
        }
    }
    NodePtr Bitwise() {
        NodePtr a = Additive();
        for (;;) {
            if (Accept("&")) {
                a = Bin("&", a, Additive());
            } else if (Accept("|")) {
                a = Bin("|", a, Additive());
            } else if (Accept("<<")) {
                a = Bin("<<", a, Additive());
            } else if (Accept(">>")) {
                a = Bin(">>", a, Additive());
            } else {
                return a;
            }
        }
    }
    NodePtr Additive() {
        NodePtr a = Multiplicative();
        for (;;) {
            if (Accept("+")) {
                a = Bin("+", a, Multiplicative());
            } else if (Accept("-")) {
                a = Bin("-", a, Multiplicative());
            } else {
                return a;
            }
        }
    }
    NodePtr Multiplicative() {
        NodePtr a = Unary();
        for (;;) {
            if (Accept("*")) {
                a = Bin("*", a, Unary());
            } else if (Accept("/")) {
                a = Bin("/", a, Unary());
            } else if (Accept("%")) {
                a = Bin("%", a, Unary());
            } else {
                return a;
            }
        }
    }
    NodePtr Unary() {
        if (Accept("-")) {
            auto n = std::make_shared<ExprNode>();
            n->kind = ExprNode::Kind::Unary;
            n->text = "-";
            n->args = {Unary()};
            return n;
        }
        if (Accept("!")) {
            auto n = std::make_shared<ExprNode>();
            n->kind = ExprNode::Kind::Unary;
            n->text = "!";
            n->args = {Unary()};
            return n;
        }
        return Primary();
    }
    NodePtr Primary() {
        Skip();
        if (pos >= src.size()) {
            throw std::runtime_error("unexpected end");
        }
        auto n = std::make_shared<ExprNode>();
        const char c = src[pos];
        if (c == '(') {
            ++pos;
            NodePtr inner = Ternary();
            Expect(")");
            return inner;
        }
        if (c == '[') {
            ++pos;
            NodePtr inner = Ternary();
            Expect("]");
            n->kind = ExprNode::Kind::Call;
            n->text = "u32";
            n->args = {inner};
            return n;
        }
        if (c == '\'') {
            const size_t end = src.find('\'', pos + 1);
            if (end == std::string::npos) {
                throw std::runtime_error("unterminated string");
            }
            n->kind = ExprNode::Kind::Str;
            n->text = src.substr(pos + 1, end - pos - 1);
            pos = end + 1;
            return n;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t used = 0;
            n->kind = ExprNode::Kind::Num;
            n->num = static_cast<s64>(std::stoull(src.substr(pos), &used, 0));
            pos += used;
            return n;
        }
        if (c == '$' || std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const bool var = c == '$';
            size_t start = var ? pos + 1 : pos;
            size_t end = start;
            while (end < src.size() &&
                   (std::isalnum(static_cast<unsigned char>(src[end])) || src[end] == '_')) {
                ++end;
            }
            const std::string name = src.substr(start, end - start);
            pos = end;
            if (var) {
                n->kind = ExprNode::Kind::Var;
                n->text = name;
                return n;
            }
            n->kind = ExprNode::Kind::Call;
            n->text = name;
            Expect("(");
            if (!Accept(")")) {
                do {
                    n->args.push_back(Ternary());
                } while (Accept(","));
                Expect(")");
            }
            return n;
        }
        throw std::runtime_error(std::string("unexpected '") + c + "'");
    }
};

// ---------------------------------------------------------------------------------------------
// Evaluation helpers

struct Env {
    const Expr::Context& ctx;
    const Kernel::Process* process;

    bool Valid(u32 addr, u32 size = 1) const {
        if (!process || addr < 0x1000) {
            return false;
        }
        auto& mem = ctx.system.Memory();
        return mem.IsValidVirtualAddress(*process, addr) &&
               mem.IsValidVirtualAddress(*process, addr + size - 1);
    }
    s64 Read(u32 addr, int size, bool sign) const {
        if (!Valid(addr, static_cast<u32>(size))) {
            return 0;
        }
        auto& mem = ctx.system.Memory();
        switch (size) {
        case 1:
            return sign ? static_cast<s8>(mem.Read8(addr)) : mem.Read8(addr);
        case 2:
            return sign ? static_cast<s16>(mem.Read16(addr)) : mem.Read16(addr);
        default:
            return sign ? static_cast<s32>(mem.Read32(addr)) : mem.Read32(addr);
        }
    }
    std::string Ascii(u32 addr, u32 max) const {
        std::string out;
        for (u32 i = 0; i < max; ++i) {
            const u8 c = static_cast<u8>(Read(addr + i, 1, false));
            if (c == 0) {
                break;
            }
            out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ';
        }
        return out;
    }
    /// Shift-JIS text with 0xF8 control codes; full-width ASCII is folded to ASCII.
    std::string Sjis(u32 addr, int line, u32 max = 512) const {
        std::string out;
        int cur = 0;
        for (u32 i = 0; i < max;) {
            const u8 c = static_cast<u8>(Read(addr + i, 1, false));
            if (c == 0) {
                break;
            }
            if (c == 0xF8) {
                const u8 code = static_cast<u8>(Read(addr + i + 1, 1, false));
                if (code == 0x02) {
                    break; // end of message
                }
                if (code == 0x01) {
                    if (cur == line) {
                        break;
                    }
                    ++cur;
                    i += 2;
                } else {
                    i += 4; // parameter codes (unexpanded) are skipped
                }
                continue;
            }
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF)) {
                const u8 d = static_cast<u8>(Read(addr + i + 1, 1, false));
                i += 2;
                if (cur != line) {
                    continue;
                }
                const u16 w = static_cast<u16>(c << 8 | d);
                out += FoldSjis(w);
                continue;
            }
            ++i;
            if (cur == line) {
                out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ';
            }
        }
        return out;
    }
    static std::string FoldSjis(u16 w) {
        if (w >= 0x8260 && w <= 0x8279)
            return std::string(1, static_cast<char>('A' + (w - 0x8260)));
        if (w >= 0x8281 && w <= 0x829A)
            return std::string(1, static_cast<char>('a' + (w - 0x8281)));
        if (w >= 0x824F && w <= 0x8258)
            return std::string(1, static_cast<char>('0' + (w - 0x824F)));
        switch (w) {
        case 0x8140:
            return " ";
        case 0x8141:
        case 0x8143:
            return ",";
        case 0x8142:
        case 0x8144:
            return ".";
        case 0x8146:
            return ":";
        case 0x8147:
            return ";";
        case 0x8148:
            return "?";
        case 0x8149:
            return "!";
        case 0x815E:
            return "/";
        case 0x8160:
            return "~";
        case 0x8165:
        case 0x8166:
            return "'";
        case 0x8167:
        case 0x8168:
            return "\"";
        case 0x8169:
            return "(";
        case 0x816A:
            return ")";
        case 0x816D:
            return "[";
        case 0x816E:
            return "]";
        case 0x817B:
            return "+";
        case 0x817C:
            return "-";
        case 0x8181:
            return "=";
        case 0x8183:
            return "<";
        case 0x8184:
            return ">";
        case 0x8190:
            return "$";
        case 0x8193:
            return "%";
        case 0x8194:
            return "#";
        case 0x8195:
            return "&";
        case 0x8196:
            return "*";
        case 0x8197:
            return "@";
        case 0x815B:
        case 0x815C:
        case 0x815D:
            return "-";
        default:
            return "";
        }
    }
    const std::vector<std::string>& Lines(const std::string& file) const {
        return ctx.lookup(file);
    }
    std::string Field(const std::string& file, s64 index, int field) const {
        const auto& lines = Lines(file);
        if (index < 0 || index >= static_cast<s64>(lines.size())) {
            return {};
        }
        const std::string& line = lines[static_cast<size_t>(index)];
        size_t start = 0;
        for (int f = 0; f < field; ++f) {
            start = line.find('\t', start);
            if (start == std::string::npos) {
                return {};
            }
            ++start;
        }
        const size_t end = line.find('\t', start);
        return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
};

// ---------------------------------------------------------------------------------------------
// Shin Megami Tensei IV: Apocalypse (USA) helpers. Addresses found by tracing the game's battle
// code (see dist/screen_regions for the layout that uses them).

namespace Smt4a {
constexpr u32 SaveDataPtr = 0x0057F69C;   // -> save data block (party, stock, items, flags)
constexpr u32 BattleTask = 0x005B3CE4;    // [[[BattleTask]+0x384]+0x2F8] = battle UI object
constexpr u32 SkillDescText = 0x00638F88; // current skill/item description (Shift-JIS)
constexpr u32 StockBase = 0xF4, StockStride = 0x12C, StockMax = 40;
constexpr u32 NanashiRec = 0x90; // Nanashi's skill block (same layout as stock)

u32 Save(const Env& e) {
    return static_cast<u32>(e.Read(SaveDataPtr, 4, false));
}
/// The battle UI object. Two game builds lay the battle task out differently: the object is at
/// [b+0x2F8], or [b+0x2E8] points 0x180 bytes into it. A candidate is accepted when its
/// per-member command counts (+0x5B8, stride 0x64) look sane.
u32 Obj(const Env& e) {
    const u32 a = static_cast<u32>(e.Read(BattleTask, 4, false));
    const u32 b = a ? static_cast<u32>(e.Read(a + 0x384, 4, false)) : 0;
    if (!b) {
        return 0;
    }
    auto sane = [&](u32 o) {
        if (o < 0x08000000 || o >= 0x10000000) {
            return false;
        }
        const s64 n0 = e.Read(o + 0x5B8, 4, false);
        const s64 n1 = e.Read(o + 0x5B8 + 0x64, 4, false);
        return n0 >= 1 && n0 <= 8 && n1 >= 0 && n1 <= 8;
    };
    // The object starts with a widget header whose vtables are fixed in the game code.
    auto sig = [&](u32 o) {
        return e.Read(o + 0x0C, 4, false) == 0x5288C8 && e.Read(o + 0x18, 4, false) == 0x5289DC &&
               e.Read(o + 0x24, 4, false) == 0x528C34 && sane(o) &&
               e.Read(o + 0x113FC, 4, false) <= 1;
    };
    const u32 o1 = static_cast<u32>(e.Read(b + 0x2F8, 4, false));
    if (sane(o1)) {
        return o1;
    }
    const u32 o2 = static_cast<u32>(e.Read(b + 0x2E8, 4, false)) - 0x180;
    if (sane(o2)) {
        return o2;
    }
    // Neither pointer is set in some battles: find the object by its header (cached; the scan
    // runs at most twice a second while it is missing).
    static std::mutex mtx;
    static u32 cached = 0;
    static std::chrono::steady_clock::time_point last_scan{};
    std::scoped_lock lock{mtx};
    if (cached && sig(cached)) {
        return cached;
    }
    cached = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_scan < std::chrono::milliseconds(1500)) {
        return 0;
    }
    last_scan = now;
    std::vector<u8> buf(0x10000);
    for (u32 base = 0x08400000; base < 0x08C00000; base += 0x10000) {
        if (!e.Valid(base, 0x10000)) {
            continue;
        }
        e.ctx.system.Memory().ReadBlock(*e.process, base, buf.data(), buf.size());
        for (u32 off = 0; off + 4 <= buf.size(); off += 4) {
            u32 v;
            std::memcpy(&v, buf.data() + off, 4);
            if (v == 0x5288C8 && base + off >= 0x0C && sig(base + off - 0x0C)) {
                cached = base + off - 0x0C;
                return cached;
            }
        }
    }
    return 0;
}
constexpr u32 BattleMgrPtr = 0x0057113C; // -> battle manager; live unit records inside it
constexpr u32 UnitBase = 0x200E6, UnitStride = 0x408;
// unit record (from its demon id field): +0 demon id (0 = Nanashi), +6 party slot, +0xF2 HP,
// +0xF6 max HP, +0xFA MP, +0xFE max MP (u32), +0x114 level
u32 Unit(const Env& e, s64 k) {
    if (k < 0 || k > 3) {
        return 0;
    }
    const u32 mgr = static_cast<u32>(e.Read(BattleMgrPtr, 4, false));
    return mgr ? mgr + UnitBase + static_cast<u32>(k) * UnitStride : 0;
}
s64 UnitDemon(const Env& e, s64 k) {
    const u32 u = Unit(e, k);
    return u ? e.Read(u, 2, false) : -1;
}
/// Stock record holding demon `id` (skill potency lives there)
u32 StockOf(const Env& e, s64 id) {
    const u32 save = static_cast<u32>(e.Read(SaveDataPtr, 4, false));
    if (!save || id <= 0) {
        return 0;
    }
    for (u32 i = 0; i < StockMax; ++i) {
        const u32 r = save + StockBase + i * StockStride;
        if (e.Read(r + 0x62, 2, false) == id) {
            return r;
        }
    }
    return 0;
}
/// Record of party member k (0 = Nanashi, 1..3 = demons by party position); skills at +0x34.
u32 Rec(const Env& e, s64 k) {
    if (k > 0) {
        const s64 id = UnitDemon(e, k);
        if (id > 0) {
            return StockOf(e, id);
        }
    }
    const u32 save = Save(e);
    if (!save) {
        return 0;
    }
    if (k <= 0) {
        return save + NanashiRec;
    }
    for (u32 i = 0; i < StockMax; ++i) {
        const u32 r = save + StockBase + i * StockStride;
        if (e.Read(r + 0x66, 2, false) == 0x100 + (k - 1)) {
            return r;
        }
    }
    return 0;
}
s64 Stat(const Env& e, s64 k, int which) {
    if (const u32 u = Unit(e, k); u && (k == 0 || UnitDemon(e, k) > 0)) {
        static constexpr u32 uoff[] = {0xF2, 0xFA, 0xF6, 0xFE, 0x114};
        return e.Read(u + uoff[which], which == 4 ? 2 : 4, false);
    }
    const u32 save = Save(e);
    if (!save) {
        return 0;
    }
    if (k <= 0) {
        static constexpr u32 off[] = {0x24, 0x26, 0x30, 0x32, 0x64};
        return e.Read(save + off[which], 2, false);
    }
    const u32 r = Rec(e, k);
    if (!r) {
        return 0;
    }
    static constexpr u32 off[] = {0x2A, 0x2C, 0x0E, 0x10, 0x64};
    return e.Read(r + off[which], 2, false);
}
/// List entry idx of member k's skill list: -1 Attack, -2 Shoot (Nanashi), skill id, 0 = none.
s64 Entry(const Env& e, s64 k, s64 idx) {
    if (idx == 0) {
        return -1;
    }
    s64 i = idx - 1;
    if (k == 0) {
        if (i == 0) {
            return -2;
        }
        --i;
    }
    const u32 obj = Obj(e);
    if (!obj || i < 0 || i >= 8 || k < 0 || k > 3) {
        return 0;
    }
    return e.Read(obj + 0x908 + static_cast<u32>(k) * 0x10 + static_cast<u32>(i) * 2, 2, false);
}
s64 Entries(const Env& e, s64 k) {
    s64 n = 0;
    while (n < 12 && Entry(e, k, n) != 0) {
        ++n;
    }
    return n;
}
s64 Cost(const Env& e, s64 k, s64 skill) {
    if (skill <= 0) {
        return 0;
    }
    const std::string base_s = e.Field("smt4a_skills.txt", skill, 2);
    if (base_s.empty()) {
        return 0;
    }
    s64 cost = std::stoll(base_s);
    // MP-saving apps (flags 12..15, one point each) apply to demons only
    const u32 save = Save(e);
    if (k > 0 && save) {
        const u8 apps = static_cast<u8>(e.Read(save + 0x2D68 + 1, 1, false));
        for (int b = 4; b < 8; ++b) {
            cost -= (apps >> b) & 1;
        }
    }
    // Potency: per-level effect pairs from the skill data (types 4 and 7 reduce the cost)
    const u32 r = Rec(e, k);
    s64 potency = 0;
    if (r) {
        for (u32 i = 0; i < 8; ++i) {
            if (e.Read(r + 0x34 + i * 2, 2, false) == skill) {
                potency = e.Read(r + 0x44 + i, 1, true);
                break;
            }
        }
    }
    if (potency > 0) {
        const std::string pairs = e.Field("smt4a_skills.txt", skill, 3);
        std::istringstream in(pairs);
        int type, value;
        char comma;
        for (s64 lv = 0; lv < std::min<s64>(potency, 9) && (in >> type >> comma >> value); ++lv) {
            if (type == 4 || type == 7) {
                cost -= value;
            }
        }
    }
    return std::max<s64>(cost, 1);
}
/// Commands of member k in on-screen order: 0 Skill 1 Item 2 Talk 3 Swap 4 Flee 5 Fusion
/// 6 Next 7 Partner.
std::vector<s64> Commands(const Env& e, s64 k) {
    const u32 obj = Obj(e);
    if (!obj || k < 0 || k > 3) {
        return {};
    }
    const s64 count = e.Read(obj + 0x5B8 + static_cast<u32>(k) * 0x64, 4, false);
    const bool items = e.Read(obj + 0x96C, 2, false) != 0;
    std::vector<s64> cmds;
    if (k == 0) {
        cmds = {0, 1, 2, 3, 4, 5, 6, 7};
        if (!items) {
            cmds.erase(std::find(cmds.begin(), cmds.end(), 1));
        }
        for (s64 drop : {7, 5, 2}) { // partner, fusion, talk: dropped in this order
            if (static_cast<s64>(cmds.size()) <= count) {
                break;
            }
            cmds.erase(std::find(cmds.begin(), cmds.end(), drop));
        }
    } else {
        cmds = {0, 1, 3, 4, 6};
        if (!items || static_cast<s64>(cmds.size()) > count) {
            cmds.erase(std::find(cmds.begin(), cmds.end(), 1));
        }
        if (static_cast<s64>(cmds.size()) > count) {
            cmds.erase(std::find(cmds.begin(), cmds.end(), 4));
        }
    }
    return cmds;
}
s64 ItemCount(const Env& e, s64 id) {
    const u32 save = Save(e);
    if (!save || id <= 0) {
        return 0;
    }
    if (id < 0x79F) {
        return e.Read(save + 0x2EAA + static_cast<u32>(id) * 2, 2, false);
    }
    return e.Read(save + 0x3CF8 + static_cast<u32>(id - 0x79F) * 2, 2, false);
}
} // namespace Smt4a

Value EvalNode(const ExprNode& n, const Env& env);

s64 Num(const ExprNode& n, const Env& env) {
    return EvalNode(n, env).n;
}

Value Call(const ExprNode& n, const Env& env) {
    const std::string& f = n.text;
    const auto& a = n.args;
    auto arg = [&](size_t i) { return i < a.size() ? Num(*a[i], env) : 0; };
    auto argv = [&](size_t i) { return i < a.size() ? EvalNode(*a[i], env) : Value{}; };
    if (f == "u8")
        return env.Read(static_cast<u32>(arg(0)), 1, false);
    if (f == "s8")
        return env.Read(static_cast<u32>(arg(0)), 1, true);
    if (f == "u16")
        return env.Read(static_cast<u32>(arg(0)), 2, false);
    if (f == "s16")
        return env.Read(static_cast<u32>(arg(0)), 2, true);
    if (f == "u32")
        return env.Read(static_cast<u32>(arg(0)), 4, false);
    if (f == "s32")
        return env.Read(static_cast<u32>(arg(0)), 4, true);
    if (f == "time")
        return static_cast<s64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    if (f == "min")
        return std::min(arg(0), arg(1));
    if (f == "max")
        return std::max(arg(0), arg(1));
    if (f == "abs")
        return std::abs(arg(0));
    if (f == "clamp")
        return std::clamp(arg(0), arg(1), std::max(arg(1), arg(2)));
    if (f == "str")
        return Value(
            env.Ascii(static_cast<u32>(arg(0)), a.size() > 1 ? static_cast<u32>(arg(1)) : 128));
    if (f == "sjis")
        return Value(env.Sjis(static_cast<u32>(arg(0)), static_cast<int>(arg(1))));
    if (f == "lookup") {
        const auto& lines = env.Lines(argv(0).Text());
        const s64 i = arg(1);
        return Value(i >= 0 && i < static_cast<s64>(lines.size()) ? lines[static_cast<size_t>(i)]
                                                                  : std::string{});
    }
    if (f == "field")
        return Value(env.Field(argv(0).Text(), arg(1), static_cast<int>(arg(2))));
    if (f == "recent") {
        // true while arg0 is true and for arg1 ms after it last was (smooths brief state flips)
        static std::mutex m;
        static std::map<const ExprNode*, std::chrono::steady_clock::time_point> seen;
        const auto now = std::chrono::steady_clock::now();
        const bool v = argv(0).Truthy();
        std::scoped_lock lock{m};
        if (v) {
            seen[&n] = now;
            return 1;
        }
        const auto it = seen.find(&n);
        return static_cast<s64>(it != seen.end() &&
                                now - it->second < std::chrono::milliseconds(arg(1)));
    }
    if (f == "track" || f == "trackage") {
        // track(key, value, ms): the summed change of value over a burst of changes, while the
        // last change is younger than ms (0 otherwise). trackage(key): ms since that change.
        struct T {
            s64 last = 0;
            s64 accum = 0;
            std::chrono::steady_clock::time_point t{};
            bool init = false;
        };
        static std::mutex m;
        static std::map<std::string, T> tracks;
        const auto now = std::chrono::steady_clock::now();
        const std::string key = argv(0).Text();
        std::scoped_lock lock{m};
        auto& t = tracks[key];
        if (f == "trackage") {
            if (!t.init || t.t == std::chrono::steady_clock::time_point{}) {
                return 1000000;
            }
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - t.t).count();
        }
        const s64 v = arg(1);
        const auto window = std::chrono::milliseconds(arg(2));
        if (!t.init) {
            t.init = true;
            t.last = v;
            return 0;
        }
        if (v != t.last) {
            t.accum = (now - t.t < window ? t.accum : 0) + (v - t.last);
            t.t = now;
            t.last = v;
        }
        return (t.t != std::chrono::steady_clock::time_point{} && now - t.t < window) ? t.accum : 0;
    }
    if (f == "has")
        return static_cast<s64>(argv(0).Text().find(argv(1).Text()) != std::string::npos);
    if (f == "pix") {
        // bottom-screen pixel 0xRRGGBB (sampled by the renderer)
        const u32 x = static_cast<u32>(std::clamp<s64>(arg(0), 0, 319));
        const u32 y = static_cast<u32>(std::clamp<s64>(arg(1), 0, 239));
        Probes::Instance().Request(x, y);
        return static_cast<s64>(Probes::Instance().Get(x, y));
    }
    if (f == "len")
        return static_cast<s64>(argv(0).Text().size());
    // --- SMT4A -------------------------------------------------------------------------------
    if (f == "smt4a_save")
        return Smt4a::Save(env);
    if (f == "smt4a_obj")
        return Smt4a::Obj(env);
    if (f == "smt4a_rec")
        return Smt4a::Rec(env, arg(0));
    if (f == "smt4a_prec") {
        // party member k's stock record from the save alone (outside battle)
        const u32 save = Smt4a::Save(env);
        const s64 k = arg(0);
        if (!save || k < 1 || k > 3) {
            return 0;
        }
        for (u32 i = 0; i < Smt4a::StockMax; ++i) {
            const u32 r = save + Smt4a::StockBase + i * Smt4a::StockStride;
            if (env.Read(r + 0x66, 2, false) == 0x100 + (k - 1)) {
                return static_cast<s64>(r);
            }
        }
        return 0;
    }
    if (f == "smt4a_hp")
        return Smt4a::Stat(env, arg(0), 0);
    if (f == "smt4a_mp")
        return Smt4a::Stat(env, arg(0), 1);
    if (f == "smt4a_maxhp")
        return Smt4a::Stat(env, arg(0), 2);
    if (f == "smt4a_maxmp")
        return Smt4a::Stat(env, arg(0), 3);
    if (f == "smt4a_level")
        return Smt4a::Stat(env, arg(0), 4);
    if (f == "smt4a_unit")
        return Smt4a::Unit(env, arg(0));
    if (f == "smt4a_demon") {
        if (arg(0) > 0 && Smt4a::Unit(env, arg(0))) {
            const s64 id = Smt4a::UnitDemon(env, arg(0));
            return id > 0 ? id : -1;
        }
        const u32 r = arg(0) <= 0 ? 0 : Smt4a::Rec(env, arg(0));
        return r ? env.Read(r + 0x62, 2, false) : -1;
    }
    if (f == "smt4a_entry")
        return Smt4a::Entry(env, arg(0), arg(1));
    if (f == "smt4a_entries")
        return Smt4a::Entries(env, arg(0));
    if (f == "smt4a_cost")
        return Smt4a::Cost(env, arg(0), arg(1));
    if (f == "smt4a_skillname") {
        const s64 id = arg(0);
        if (id == -1)
            return Value(std::string("Attack"));
        if (id == -2)
            return Value(std::string("Shoot"));
        return Value(env.Field("smt4a_skills.txt", id, 1));
    }
    if (f == "smt4a_skillicon") {
        const s64 id = arg(0);
        if (id < 0)
            return id == -1 ? 1 : 3; // Attack: phys, Shoot: gun
        const std::string s = env.Field("smt4a_skills.txt", id, 4);
        return s.empty() ? 0 : std::stoll(s);
    }
    if (f == "smt4a_cmd") {
        const auto cmds = Smt4a::Commands(env, arg(0));
        const s64 i = arg(1);
        return i >= 0 && i < static_cast<s64>(cmds.size()) ? cmds[static_cast<size_t>(i)] : -1;
    }
    if (f == "smt4a_cmdcount")
        return static_cast<s64>(Smt4a::Commands(env, arg(0)).size());
    if (f == "smt4a_item") {
        const u32 obj = Smt4a::Obj(env);
        return obj ? env.Read(obj + 0x96C + static_cast<u32>(arg(0)) * 2, 2, false) : 0;
    }
    if (f == "smt4a_itemcount")
        return Smt4a::ItemCount(env, arg(0));
    if (f == "smt4a_itemname")
        return Value(env.Field("smt4a_items.txt", arg(0), 0));
    if (f == "smt4a_skilldesc")
        return Value(env.Sjis(Smt4a::SkillDescText, static_cast<int>(arg(0))));
    if (f == "smt4a_cmddesc") {
        const u32 obj = Smt4a::Obj(env);
        return Value(obj ? env.Ascii(obj + 0x11370 + static_cast<u32>(arg(0)) * 0x40, 64)
                         : std::string{});
    }
    throw std::runtime_error("unknown function " + f);
}

Value EvalNode(const ExprNode& n, const Env& env) {
    switch (n.kind) {
    case ExprNode::Kind::Num:
        return n.num;
    case ExprNode::Kind::Str:
        return Value(n.text);
    case ExprNode::Kind::Var: {
        const auto it = env.ctx.vars.find(n.text);
        return it == env.ctx.vars.end() ? Value{} : it->second;
    }
    case ExprNode::Kind::Unary: {
        const Value v = EvalNode(*n.args[0], env);
        return n.text == "-" ? Value(-v.n) : Value(static_cast<s64>(!v.Truthy()));
    }
    case ExprNode::Kind::Ternary:
        return EvalNode(*n.args[0], env).Truthy() ? EvalNode(*n.args[1], env)
                                                  : EvalNode(*n.args[2], env);
    case ExprNode::Kind::Call:
        return Call(n, env);
    case ExprNode::Kind::Binary: {
        const std::string& op = n.text;
        if (op == "&&") {
            return static_cast<s64>(EvalNode(*n.args[0], env).Truthy() &&
                                    EvalNode(*n.args[1], env).Truthy());
        }
        if (op == "||") {
            return static_cast<s64>(EvalNode(*n.args[0], env).Truthy() ||
                                    EvalNode(*n.args[1], env).Truthy());
        }
        const Value a = EvalNode(*n.args[0], env);
        const Value b = EvalNode(*n.args[1], env);
        if (a.is_str || b.is_str) {
            if (op == "+")
                return Value(a.Text() + b.Text());
            if (op == "==")
                return static_cast<s64>(a.Text() == b.Text());
            if (op == "!=")
                return static_cast<s64>(a.Text() != b.Text());
            return Value{};
        }
        const s64 x = a.n, y = b.n;
        if (op == "+")
            return x + y;
        if (op == "-")
            return x - y;
        if (op == "*")
            return x * y;
        if (op == "/")
            return y ? x / y : 0;
        if (op == "%")
            return y ? x % y : 0;
        if (op == "==")
            return static_cast<s64>(x == y);
        if (op == "!=")
            return static_cast<s64>(x != y);
        if (op == "<")
            return static_cast<s64>(x < y);
        if (op == ">")
            return static_cast<s64>(x > y);
        if (op == "<=")
            return static_cast<s64>(x <= y);
        if (op == ">=")
            return static_cast<s64>(x >= y);
        if (op == "&")
            return x & y;
        if (op == "|")
            return x | y;
        if (op == "<<")
            return x << (y & 63);
        if (op == ">>")
            return x >> (y & 63);
        return Value{};
    }
    }
    return Value{};
}

} // namespace

bool Expr::Parse(const std::string& text, std::string& error) {
    try {
        Parser p(text);
        root = p.ParseAll();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        root.reset();
        return false;
    }
}

Value Expr::Eval(const Context& ctx) const {
    if (!root) {
        return Value{};
    }
    const auto process = ctx.system.Kernel().GetCurrentProcess();
    Env env{ctx, process.get()};
    try {
        return EvalNode(*root, env);
    } catch (const std::exception&) {
        return Value{};
    }
}

} // namespace ScreenRegions
