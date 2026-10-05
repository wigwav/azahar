// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Small expression language for HUD bindings.
//
//   numbers 12 0x1F, strings 'text', $variables
//   + - * / % == != < > <= >= && || ! unary-, a ? b : c, ( )
//   [addr]            u32 read
//   u8() s8() u16() s16() u32() s32()   memory reads
//   min() max() clamp(v,lo,hi) abs()
//   str(addr[,max])   ASCII string at addr
//   sjis(addr[,line]) Shift-JIS text (full-width ASCII folded), line N of a 0xF801-separated text
//   lookup('file.txt', index)    line of a text file in the HUD folder
//   game helpers (see hud_expr.cpp), e.g. smt4a_cost(member, skill)

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace ScreenRegions {

struct Value {
    bool is_str = false;
    s64 n = 0;
    std::string s;

    Value() = default;
    Value(s64 v) : n(v) {}
    Value(std::string v) : is_str(true), s(std::move(v)) {}

    bool Truthy() const {
        return is_str ? !s.empty() : n != 0;
    }
    std::string Text() const {
        return is_str ? s : std::to_string(n);
    }
    bool operator==(const Value& o) const {
        return is_str == o.is_str && n == o.n && s == o.s;
    }
};

struct ExprNode;

class Expr {
public:
    /// Parses `text`; on failure returns false and sets `error`.
    bool Parse(const std::string& text, std::string& error);
    bool Empty() const {
        return root == nullptr;
    }

    struct Context {
        Core::System& system;
        const std::map<std::string, Value>& vars;
        /// Loads a text file from the HUD folder (lines), cached by the caller.
        std::function<const std::vector<std::string>&(const std::string&)> lookup;
    };
    Value Eval(const Context& ctx) const;

private:
    std::shared_ptr<ExprNode> root;
};

} // namespace ScreenRegions
