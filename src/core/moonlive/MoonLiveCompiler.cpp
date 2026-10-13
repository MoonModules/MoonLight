/// The MoonLive compiler: source text to IR, in a lexer, a parser and a code generator.
/// Each stage is a section below, and the emitters it feeds are per-architecture.

#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

#include <cstring>   // std::strncmp (keyword matching)

namespace mm::moonlive {

namespace {

// --- Lexer ---------------------------------------------------------------------------

// A `//` line comment is whitespace, and `Assign` is a declaration's initializer.
/// Every token the grammar has.
enum class Tok { Ident, Number, String, Assign, LParen, RParen, LBrace, RBrace, Comma, Semicolon,
                 Plus, Minus, Star, Slash, Percent, Less, LessEq, Greater, GreaterEq, EqEq, NotEq,
                 LBracket, RBracket, Dot, AndAnd, OrOr, Not, End, Error };

/// One token at a time over the source, with no buffer of its own.
struct Lexer {
    const char* p;                    ///< the read cursor
    Tok kind = Tok::Error;            ///< what the current token is
    int64_t number = 0;               ///< its value, when it is a number
    bool numberIsFixed = false;       ///< whether that literal carried a decimal point
    const char* identBeg = nullptr;   ///< an identifier or string span into the source
    size_t identLen = 0;              ///< how long that span is
    const char* tokBeg = nullptr;     ///< where the current token started
    const char* srcBeg;               ///< where the source started, for the column
    const char* err = "";             ///< why lexing failed

    /// Start at the first token of a source buffer.
    explicit Lexer(const char* s) : p(s), srcBeg(s) { advance(); }

    /// Whether a character separates tokens.
    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    /// Whether a character is a decimal digit.
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    /// Whether a character can open an identifier.
    static bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    /// Whether a character can continue one.
    static bool isIdentCont(char c) { return isIdentStart(c) || isDigit(c); }
    /// The current token's one-based column, for a diagnostic.
    uint16_t col() const { return static_cast<uint16_t>((tokBeg - srcBeg) + 1); }

    // A decimal point makes it fixed, and overflow fails rather than truncating.
    /// Read a number, saying whether it was fixed and whether it overflowed.
    bool readNumber(int64_t& v, bool& isFixed, bool& overflowed) {
        if (!isDigit(*p)) return false;
        v = 0; isFixed = false; overflowed = false;
        // int64 throughout, since `long` is 32 bits where a script compiles.
        while (isDigit(*p)) {
            if (v > (INT64_MAX - 9) / 10) { overflowed = true; return true; }
            v = v * 10 + (*p - '0');
            p++;
            // The magnitude, since a leading minus is a separate token checked where it is known.
            if (v > -static_cast<int64_t>(INT32_MIN)) { overflowed = true; return true; }
        }
        if (*p == '.' && isDigit(p[1])) {
            isFixed = true;
            p++;
            int64_t num = 0, den = 1;
            while (isDigit(*p)) {
                // Bounded to keep the scaling inside int64; further digits are consumed and dropped.
                if (den <= 100000000LL) { num = num * 10 + (*p - '0'); den *= 10; }
                p++;
            }
            // The magnitude again, since 32768.0 is legal only with the leading minus.
            if (v > 32768) { overflowed = true; return true; }
            // The integer part scales by 65536; the fraction is num/den of that, rounded.
            v = (v << 16) + (num * 65536 + den / 2) / den;
        }
        return true;
    }

    /// Lex the next token into this lexer's fields.
    void advance() {
        for (;;) {
            while (isSpace(*p)) p++;
            // A line comment is whitespace, with no exception.
            if (p[0] == '/' && p[1] == '/') {
                p += 2;
                // Whitespace with no exception, since a comment that changed behavior is not C.
                while (*p && *p != '\n') p++;
                continue;
            }
            break;
        }
        tokBeg = p;
        char c = *p;
        if (c == 0) { kind = Tok::End; return; }
        // Maximal munch: a short form tested first would lex `a == b` as two assignments.
        if (c == '<' && p[1] == '=') { p += 2; kind = Tok::LessEq;    return; }
        if (c == '>' && p[1] == '=') { p += 2; kind = Tok::GreaterEq; return; }
        if (c == '=' && p[1] == '=') { p += 2; kind = Tok::EqEq;      return; }
        if (c == '!' && p[1] == '=') { p += 2; kind = Tok::NotEq;     return; }
        if (c == '&' && p[1] == '&') { p += 2; kind = Tok::AndAnd;    return; }
        if (c == '|' && p[1] == '|') { p += 2; kind = Tok::OrOr;      return; }
        if (c == '!') { p++; kind = Tok::Not; return; }
        // A field access; a number's own point is read with the number, which starts with a digit.
        if (c == '.') { p++; kind = Tok::Dot; return; }
        if (c == '=') { p++; kind = Tok::Assign; return; }
        if (c == '(') { p++; kind = Tok::LParen; return; }
        if (c == ')') { p++; kind = Tok::RParen; return; }
        if (c == ',') { p++; kind = Tok::Comma; return; }
        if (c == ';') { p++; kind = Tok::Semicolon; return; }
        if (c == '+') { p++; kind = Tok::Plus;    return; }
        if (c == '-') { p++; kind = Tok::Minus;   return; }
        if (c == '*') { p++; kind = Tok::Star;    return; }
        if (c == '{') { p++; kind = Tok::LBrace;  return; }
        if (c == '}') { p++; kind = Tok::RBrace;  return; }
        if (c == '[') { p++; kind = Tok::LBracket; return; }
        if (c == ']') { p++; kind = Tok::RBracket; return; }
        if (c == '<') { p++; kind = Tok::Less;    return; }
        if (c == '>') { p++; kind = Tok::Greater; return; }
        // '/' only reaches here when it is NOT the `//` a comment starts with (handled above).
        if (c == '/') { p++; kind = Tok::Slash;   return; }
        if (c == '%') { p++; kind = Tok::Percent; return; }
        // A control's UI label, spanned like an identifier since both are source bytes; no escapes.
        if (c == '"') {
            p++;
            identBeg = p;
            while (*p && *p != '"' && *p != '\n') p++;
            if (*p != '"') { kind = Tok::Error; err = "unterminated string"; return; }
            identLen = static_cast<size_t>(p - identBeg);
            p++;                                    // the closing quote
            kind = Tok::String; return;
        }
        if (isDigit(c)) {
            int64_t v = 0; bool fx = false, over = false;
            readNumber(v, fx, over);
            if (over) { err = "number out of range"; kind = Tok::Error; return; }
            number = v; numberIsFixed = fx; kind = Tok::Number; return;
        }
        if (isIdentStart(c)) {
            identBeg = p;
            while (isIdentCont(*p)) p++;
            identLen = static_cast<size_t>(p - identBeg);
            kind = Tok::Ident; return;
        }
        kind = Tok::Error; err = "unexpected character";
    }
};

// --- Parser to IR --------------------------------------------------------------------

// It knows the grammar and resolves call names against the injected table, owning no function name.
/// What a condition operator met where a value is wanted says.
constexpr const char* kJoinsConditions = "&&, || and ! join the conditions of an if, not values";
/// What a whole struct where one value belongs says.
constexpr const char* kOneFieldAtATime = "a struct is read one field at a time: write name.field";
/// What a comparison after a negated value says.
constexpr const char* kNegatesWhatFollows = "! negates what follows it: write !(a < b) to negate a comparison";

/// Recursive descent, evaluating each expression into a virtual register and emitting IR.
struct Parser {
    Lexer&             lex;       ///< the token stream being parsed
    const BuiltinTable& table;    ///< the functions the host registered
    const SysVarTable&  sysvars;  ///< the system variables the host defines
    IrProgram&         ir;        ///< where the emitted ops go
    char*              classNameOut = nullptr;   ///< the caller's buffer, which the parser fills
    // An IR index rather than a byte offset, since the parser runs before lowering.
    /// One function the class defined, and where its body starts.
    struct FnMark { const char* name;    ///< its name, a span into the source
                    uint8_t nameLen;     ///< how long that span is
                    uint16_t irStart;    ///< the IR index its body starts at
                    RetType ret;         ///< what it declares it returns
                    uint8_t params;      ///< how many argument words it declares
                    uint8_t retStruct;   ///< the struct it returns, as its table index plus one, or 0
                    uint8_t argCount;    ///< how many parameters it declares, a struct counting once
                    uint8_t argStruct[moonlive::kMaxScriptArgs]; };   ///< each parameter's struct, its table index plus one, or 0 for a scalar
    FnMark             fns[kMaxEntryPoints] = {};   ///< the functions parsed so far
    uint8_t            fnCount = 0;                 ///< how many of them there are
    /// What the function being parsed returns, so a `return` is checked where it is written.
    RetType            curRet = RetType::Void;
    /// The struct it returns, when it returns one.
    const StructDef*   curRetStruct = nullptr;
    /// Each argument word's type, so the entry copy narrows a byte field as it narrows a byte parameter.
    CtrlType           paramWordType[kMaxScriptArgs] = {};
    VReg               nextTemp = kFirstTemp;       ///< the high-water mark, also vregsUsed
    VReg               freeStack[kMaxVRegs] = {};   ///< recycled temps, so a dead vreg is reused
    uint8_t            freeCount = 0;               ///< how many are on that stack
    // In a frame slot rather than a register, so the live count is bounded by memory.
    /// A script-local variable, such as a `for` counter, which the script writes.
    struct Local { const char* name;   ///< its name, a span into the source
                   size_t nameLen;     ///< how long that span is
                   uint8_t slot;       ///< the frame slot holding it, the first field's for a struct
                   CtrlType type;      ///< its declared type, a struct's field type
                   uint8_t st = 0; };  ///< its struct type, as its table index plus one, or 0 for a scalar
    Local              locals[kMaxLocals] = {};   ///< the locals currently in scope
    uint8_t            localCount = 0;            ///< how many of them there are
    uint8_t            slotHighWater = 0;         ///< slots currently in scope
    uint8_t            slotsUsed = 0;             ///< peak slots, what the prologue reserves
    uint8_t            nextLabel = 0;             ///< IR label ids, in source order

    // Whether the UI shows a member is decided by addControl, so a control's offset is its byte.
    char*              strings = nullptr;         ///< the caller's string pool
    uint16_t           stringCap = 0;             ///< its capacity
    uint16_t           stringLen = 0;             ///< how much of it is used
    DeclaredControl    members[kMaxCtrls] = {};   ///< the members declared so far
    uint8_t            memberStruct[kMaxCtrls] = {};   ///< a member's struct type, as its table index plus one, or 0
    // No AST, so the type rides alongside the register and every parse function sets it.
    /// Whether the value last parsed is Q16.16 rather than a plain integer.
    bool               exprIsFixed = false;
    /// Whether a same-type builtin's arguments, last parsed, were fixed, which its result follows.
    bool               argsFixed = false;
    // A literal converts by patching its Const, since its meaning is visible at the call site.
    /// The Const op index when the value last parsed is one integer literal, else -1.
    int                exprLitConst = -1;

    uint8_t            memberBytes = 0;   ///< arena bytes the members occupy, the placement cursor
    uint8_t            memberCount = 0;   ///< how many members are declared

    const char*        error = "";        ///< why the compile failed
    uint16_t           errorCol = 0;      ///< the one-based column it failed at
    bool               failed = false;    ///< whether it failed at all

    /// Record the first failure, with where it happened.
    void fail(const char* msg) { if (!failed) { failed = true; error = msg; errorCol = lex.col(); } }


    // The source is freed on return, so the pool travels with the program instead.
    /// Copy a token's text into the program's string pool.
    const char* internString(const char* text, size_t len) {
        if (!strings || stringLen + len + 1 > stringCap) return nullptr;
        char* at = strings + stringLen;
        for (size_t i = 0; i < len; i++) at[i] = text[i];
        at[len] = '\0';
        stringLen = static_cast<uint16_t>(stringLen + len + 1);
        return at;
    }


    /// Find a script-local by name; its index, or -1.
    int findLocal(const char* name, size_t len) const {
        for (uint8_t i = 0; i < localCount; i++)
            if (locals[i].nameLen == len && std::strncmp(locals[i].name, name, len) == 0)
                return i;
        return -1;
    }

    // A frame slot has no narrowing store, so shifting up and back down truncates instead.
    /// Truncate a value to the width its type promises.
    void narrowToType(VReg v, CtrlType type) {
        if (type != CtrlType::Byte && type != CtrlType::Bool) return;
        emit({IrOp::Shl, v, v, 0,0,0, 24, nullptr, {}});
        emit({IrOp::Shr, v, v, 0,0,0, 24, nullptr, {}});
    }

    // The same bounds a member's initializer takes, so `byte b = 300;` is refused everywhere.
    /// Whether a compile-time constant fits the range its type promises.
    static bool fitsInType(int64_t v, CtrlType type) {
        switch (type) {
            case CtrlType::Byte: return v >= 0 && v <= 255;
            case CtrlType::Bool: return v >= 0 && v <= 1;
            default:             return true;      // int and fixed take the whole slot
        }
    }

    /// Find a declared member by name; its index, or -1.
    int findMember(const char* name, size_t len) const {
        for (uint8_t i = 0; i < memberCount; i++)
            if (members[i].nameLen == len && std::strncmp(members[i].name, name, len) == 0)
                return i;
        return -1;
    }

    // --- Structs: a host-registered type is its fields under one name, a named array ---

    // A byte rather than a pointer in every local, member and function record, since the parser lives on the compiling task's stack.
    /// The struct a stored index names, or null for 0.
    const StructDef* structAt(uint8_t idx) const { return idx ? &table.structs[idx - 1] : nullptr; }
    /// The index a struct is stored as: its place in the table plus one, or 0 for none.
    uint8_t structIndex(const StructDef* st) const { return st ? static_cast<uint8_t>(st - table.structs + 1) : 0; }

    /// A field's index in a struct, or -1.
    static int fieldIndex(const StructDef& st, const char* n, size_t len) {
        for (uint8_t f = 0; f < st.fieldCount; f++)
            if (std::strlen(st.fields[f]) == len && std::strncmp(st.fields[f], n, len) == 0) return f;
        return -1;
    }

    /// Parse `.field` after a struct's name, answering the field's index, or -1 having failed.
    int parseField(const StructDef& st) {
        if (!expect(Tok::Dot, kOneFieldAtATime)) return -1;
        if (lex.kind != Tok::Ident) { fail("expected a field name after '.'"); return -1; }
        const int f = fieldIndex(st, lex.identBeg, lex.identLen);
        if (f < 0) { fail("this struct has no field of that name"); return -1; }
        lex.advance();
        return f;
    }

    /// The arena byte of a struct member's field.
    uint8_t memberFieldOffset(int mi, int f) const {
        return static_cast<uint8_t>(members[mi].offset + f * ctrlWidth(members[mi].type));
    }

    /// Load one field of a struct member, at its own width.
    VReg loadMemberField(int mi, int f) {
        VReg v = alloc();
        emit({ctrlWidth(members[mi].type) == 1 ? IrOp::LoadCtrl : IrOp::LoadCtrl32, v, 0,0,0,0,
              memberFieldOffset(mi, f), nullptr, {}});
        return v;
    }

    // Peeks without consuming, so a caller can ask before it commits to a scalar or a struct.
    /// The struct type the expression at the cursor has as a whole value, or null for a scalar.
    const StructDef* structValueAt() const {
        if (lex.kind != Tok::Ident) return nullptr;
        Lexer probe = lex;
        probe.advance();
        // A call or constructor is followed past its parentheses, so `getRGB(i).r` reads as the scalar it is.
        if (probe.kind == Tok::LParen) {
            // A lexer error does not move past its character, so it ends the probe as the end does.
            for (int depth = 0; probe.kind != Tok::End && probe.kind != Tok::Error; ) {
                if (probe.kind == Tok::LParen) depth++;
                if (probe.kind == Tok::RParen && --depth == 0) { probe.advance(); break; }
                probe.advance();
            }
        }
        if (probe.kind == Tok::Dot) return nullptr;   // `c.r` is one field, a scalar
        if (const StructDef* st = table.findStruct(lex.identBeg, lex.identLen)) return st;   // a constructor
        const int li = findLocal(lex.identBeg, lex.identLen);
        if (li >= 0) return structAt(locals[li].st);
        const int mi = findMember(lex.identBeg, lex.identLen);
        if (mi >= 0) return structAt(memberStruct[mi]);
        return structCallAt();   // a builtin or a script function answering one
    }

    /// Keep a struct answer's slots, the first `st.fieldCount` from `argBase`, which the builtin writes past its arguments; false when they do not fit.
    bool reserveStructAnswer(uint8_t argBase, const StructDef& st) {
        if (argBase + st.fieldCount > kMaxLocals) { fail("too many arguments to hold"); return false; }
        if (slotHighWater < argBase + st.fieldCount) slotHighWater = static_cast<uint8_t>(argBase + st.fieldCount);
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        return true;
    }

    /// The struct a call or constructor at the cursor answers, read through a following `.field`.
    const StructDef* structCallAt() const {
        if (lex.kind != Tok::Ident) return nullptr;
        if (const StructDef* st = table.findStruct(lex.identBeg, lex.identLen)) return st;
        if (const Builtin* b = table.find(lex.identBeg, lex.identLen))
            return b->retStruct ? table.findStruct(b->retStruct, std::strlen(b->retStruct)) : nullptr;
        for (uint8_t i = 0; i < fnCount; i++)
            if (fns[i].nameLen == lex.identLen && std::strncmp(fns[i].name, lex.identBeg, lex.identLen) == 0)
                return structAt(fns[i].retStruct);
        return nullptr;
    }

    // Every source of a struct value lands in registers, one per field, so every destination stores the same way.
    /// Parse a whole struct value of type `st` into one register per field.
    bool parseStructValue(const StructDef& st, VReg out[kMaxStructFields], bool thenField = false) {
        const StructDef* got = thenField ? structCallAt() : structValueAt();
        if (got != &st) {
            fail(got ? "this is a different struct type"
                     : "expected a struct value: a constructor, a struct variable or a call answering one");
            return false;
        }
        if (table.findStruct(lex.identBeg, lex.identLen)) {     // `RGB(r, g, b)`: one expression per field
            lex.advance();
            if (!expect(Tok::LParen, "expected '(' after the struct type")) return false;
            for (uint8_t f = 0; f < st.fieldCount; f++) {
                if (f > 0 && !expect(Tok::Comma, "expected ',' between the fields")) return false;
                out[f] = parseExpr();
                if (failed) return false;
                if (exprIsFixed) { fail("a struct field is a whole number: write toInt(x)"); return false; }
                if (exprLitConst >= 0 && !fitsInType(ir.ops[exprLitConst].imm, st.type))
                    { fail("this number is out of range for that field"); return false; }
                narrowToType(out[f], st.type);
            }
            if (lex.kind == Tok::Comma) { fail("too many fields for this struct"); return false; }
            return expect(Tok::RParen, "expected ')' to close the struct");
        }
        const int li = findLocal(lex.identBeg, lex.identLen);
        if (li >= 0) {
            lex.advance();
            for (uint8_t f = 0; f < st.fieldCount; f++) {
                out[f] = alloc();
                emit({IrOp::Reload, out[f], 0,0,0,0, locals[li].slot + f, nullptr, {}});
            }
            return true;
        }
        const int mi = findMember(lex.identBeg, lex.identLen);
        if (mi >= 0) {
            lex.advance();
            for (uint8_t f = 0; f < st.fieldCount; f++) out[f] = loadMemberField(mi, f);
            return true;
        }
        if (const Builtin* b = table.find(lex.identBeg, lex.identLen)) {
            // The builtin writes its fields into its first argument slots, which are reserved even past its arguments.
            lex.advance();
            if (!expect(Tok::LParen, "expected '(' after the function name")) return false;
            const uint8_t argBase = slotHighWater;
            uint8_t n = 0;
            if (!parseBuiltinArgs(b, n)) return false;
            if (!reserveStructAnswer(argBase, st)) return false;
            VReg r = alloc();
            emit({IrOp::Call, r, 0, n, 0, 0, argBase, b->fn, {}});
            freeTemp(r);
            for (uint8_t f = 0; f < st.fieldCount; f++) {
                out[f] = alloc();
                emit({IrOp::Reload, out[f], 0,0,0,0, argBase + f, nullptr, {}});
            }
            slotHighWater = argBase;
            return true;
        }
        // A script function answering a struct leaves its fields in the argument block, free once the callee copied its own.
        for (uint8_t i = 0; i < fnCount; i++) {
            if (fns[i].nameLen != lex.identLen || std::strncmp(fns[i].name, lex.identBeg, lex.identLen) != 0) continue;
            lex.advance();
            if (!expect(Tok::LParen, "expected '(' after the function name")) return false;
            if (!stageScriptArgs(i)) return false;
            emit({IrOp::CallScript, 0, 0,0,0,0, static_cast<int32_t>(i), nullptr, {}});
            for (uint8_t f = 0; f < st.fieldCount; f++) {
                out[f] = alloc();
                emit({IrOp::LoadCtrl32, out[f], 0,0,0,0, scriptArgOffset(f), nullptr, {}});
            }
            return true;
        }
        fail("expected a struct value");
        return false;
    }

    // Stored once all are evaluated, since a later argument may call through the same block.
    /// Parse a script call's arguments through its ')' into the arena argument block, a struct taking one word per field.
    bool stageScriptArgs(uint8_t i) {
        const uint8_t want = fns[i].params;
        VReg staged[kMaxScriptArgs] = {};
        uint8_t got = 0, args = 0;
        while (lex.kind != Tok::RParen && lex.kind != Tok::End) {
            if (args > 0 && !expect(Tok::Comma, "expected ',' between arguments")) return false;
            VReg vals[kMaxStructFields] = {};
            uint8_t count = 1;
            // Each argument against its own parameter, as C++ checks a call, so a scalar or another struct cannot pass for the one declared.
            const bool declared = args < fns[i].argCount;
            const StructDef* param = declared ? structAt(fns[i].argStruct[args]) : structValueAt();
            if (param) {
                if (!parseStructValue(*param, vals)) return false;
                count = param->fieldCount;
            } else {
                if (structValueAt()) { fail("a struct is read one field at a time: write name.field"); return false; }
                vals[0] = parseExpr();
                if (failed) return false;
            }
            for (uint8_t f = 0; f < count; f++) {
                if (got < want && got < kMaxScriptArgs) staged[got] = vals[f];
                else freeTemp(vals[f]);
                got++;
            }
            args++;
        }
        if (!expect(Tok::RParen, "expected ')' to close the arguments")) return false;
        const uint8_t staging = got < want ? got : want;
        for (uint8_t a = 0; a < staging && a < kMaxScriptArgs; a++) {
            emit({IrOp::StoreCtrl32, 0, staged[a], 0,0,0, scriptArgOffset(a), nullptr, {}});
            freeTemp(staged[a]);
        }
        if (got != want) {
            fail(got < want ? "too few arguments for this function" : "too many arguments for this function");
            return false;
        }
        return true;
    }

    // The textbook tree-walk register stack, so a multi-call statement fits the register file.
    /// Hand out a recycled virtual register, or a fresh one.
    VReg alloc() {
        if (freeCount) return freeStack[--freeCount];
        if (nextTemp < kMaxVRegs) return nextTemp++;
        // Out of registers: fail rather than aliasing the last vreg into wrong IR.
        fail("script too complex (out of registers)");
        return kFirstTemp;
    }
    /// Return a temp to the pool once its value is consumed.
    void freeTemp(VReg v) {
        // Every vreg reaching here is a temp, since a variable lives in a frame slot.
        if (v >= kFirstTemp && freeCount < kMaxVRegs) freeStack[freeCount++] = v;
    }

    /// Append an IR op, failing the compile when the program is full or the vreg is out of budget.
    void emit(const IrInst& i) { if (!ir.push(i)) fail("script too large"); }

    // A condition consumes its own && and ||, so one met here stands where a value was wanted.
    /// Consume the expected token, or fail with a message.
    bool expect(Tok t, const char* msg) {
        if (lex.kind != t) { fail(lex.kind == Tok::AndAnd || lex.kind == Tok::OrOr ? kJoinsConditions : msg); return false; }
        lex.advance();
        return true;
    }

    // Precedence climbing onto the IR the three backends already have, since none has a subtract.
    /// Reconcile a binary operator's operand types, answering the combined type in `outFixed`.
    bool meet(bool lhsFixed, int lhsLit, bool rhsFixed, int rhsLit, bool& outFixed) {
        if (lhsFixed == rhsFixed) { outFixed = lhsFixed; return true; }
        const int lit = lhsFixed ? rhsLit : lhsLit;    // the integer side, when it is a literal
        if (lit >= 0) {
            const int32_t v = ir.ops[lit].imm;
            if (v < -32768 || v > 32767) {
                fail("this number is out of range for a fixed value");
                return false;
            }
            ir.ops[lit].imm = v << 16;
            outFixed = true;
            return true;
        }
        fail("mixes whole and fixed: write toFixed(x) or toInt(x)");
        return false;
    }

    /// Parse an expression into a virtual register.
    VReg parseExpr() {
        VReg lhs = parseTerm();
        while (!failed && (lex.kind == Tok::Plus || lex.kind == Tok::Minus)) {
            const bool negate = (lex.kind == Tok::Minus);
            lex.advance();
            const bool lhsFixed = exprIsFixed;
            const int  lhsLit   = exprLitConst;
            VReg rhs = parseTerm();
            if (failed) return 0;
            bool outFixed = false;
            if (!meet(lhsFixed, lhsLit, exprIsFixed, exprLitConst, outFixed)) return 0;
            exprIsFixed = outFixed;
            exprLitConst = -1;                     // a combined value is not one literal
            if (negate) {
                VReg m = alloc();
                emit({IrOp::Const, m, 0,0,0,0, -1, nullptr, {}});
                VReg n = alloc();
                emit({IrOp::Mul, n, rhs, m, 0,0, 0, nullptr, {}});
                freeTemp(m); freeTemp(rhs);
                rhs = n;
            }
            VReg dst = alloc();
            emit({IrOp::Add, dst, lhs, rhs, 0,0, 0, nullptr, {}});
            freeTemp(lhs); freeTemp(rhs);
            lhs = dst;
        }
        return lhs;
    }

    // A host call, since no ISA here divides, resolved by name because core is domain-neutral.
    /// Lower a binary operator to a host call, failing with `absent` when it is not registered.
    VReg emitBinaryCall(const char* name, size_t nameLen, VReg lhs, VReg rhs, const char* absent) {
        const Builtin* fn = table.find(name, nameLen);
        if (!fn || fn->argc != 2 || !fn->returns || fn->kind != BuiltinKind::Call) {
            fail(absent); return 0;
        }
        // Consecutive frame slots, as a two-argument call stages them: nothing stays in a register.
        if (slotHighWater + 1 >= kMaxLocals) { fail("expression too deeply nested"); return 0; }
        const uint8_t argBase = slotHighWater;
        emit({IrOp::Spill, 0, lhs, 0,0,0, slotHighWater++, nullptr, {}});
        emit({IrOp::Spill, 0, rhs, 0,0,0, slotHighWater++, nullptr, {}});
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        freeTemp(lhs); freeTemp(rhs);
        VReg dst = alloc();
        emit({IrOp::Call, dst, 0, 2, 0, 0, argBase, fn->fn, {}});
        slotHighWater = argBase;               // the staging slots die with the call
        return dst;
    }

    /// Parse a term: a run of primaries joined by the tighter-binding operators.
    VReg parseTerm() {
        VReg lhs = parsePrimary();
        while (!failed && (lex.kind == Tok::Star || lex.kind == Tok::Slash
                           || lex.kind == Tok::Percent)) {
            const Tok op = lex.kind;
            const bool lhsFixed = exprIsFixed;
            const int  lhsLit   = exprLitConst;
            lex.advance();
            VReg rhs = parsePrimary();
            if (failed) return 0;
            bool bothFixed = false;
            if (!meet(lhsFixed, lhsLit, exprIsFixed, exprLitConst, bothFixed)) return 0;
            exprLitConst = -1;
            if (op == Tok::Star && bothFixed) {
                // The product has 32 fraction bits, so the answer is its middle word.
                VReg hi = alloc();
                emit({IrOp::Mulhi, hi, lhs, rhs, 0,0, 0, nullptr, {}});
                VReg lo = alloc();
                emit({IrOp::Mul, lo, lhs, rhs, 0,0, 0, nullptr, {}});
                emit({IrOp::Shr, lo, lo, 0,0,0, 16, nullptr, {}});   // LOGICAL: the low word is unsigned
                emit({IrOp::Shl, hi, hi, 0,0,0, 16, nullptr, {}});
                VReg dst = alloc();
                emit({IrOp::Add, dst, hi, lo, 0,0, 0, nullptr, {}});
                freeTemp(hi); freeTemp(lo); freeTemp(lhs); freeTemp(rhs);
                lhs = dst;
                exprIsFixed = true;
            } else if (op == Tok::Star) {
                VReg dst = alloc();
                emit({IrOp::Mul, dst, lhs, rhs, 0,0, 0, nullptr, {}});
                freeTemp(lhs); freeTemp(rhs);
                lhs = dst;
                exprIsFixed = false;
            } else if (op == Tok::Slash && bothFixed) {
                // Its own host call, since the numerator widens past what a 32-bit register holds.
                lhs = emitBinaryCall("fdiv", 4, lhs, rhs, "'/' on fixed needs the fdiv built-in");
                exprIsFixed = true;
            } else if (op == Tok::Slash) {
                lhs = emitBinaryCall("div", 3, lhs, rhs, "'/' needs a div(a, b) built-in");
                exprIsFixed = false;
            } else {
                // The remainder of two fixed values is fixed, which is what makes `x % 1.0` work.
                lhs = emitBinaryCall("mod", 3, lhs, rhs, "'%' needs a mod(a, b) built-in");
                exprIsFixed = bothFixed;
            }
            if (failed) return 0;
        }
        return lhs;
    }

    // Explicit, since a silent conversion is 65,536 times off; a shift, not a host call.
    /// Parse `toFixed(x)` or `toInt(x)`, the cursor on the keyword.
    VReg parseConversion() {
        const bool up = atKeyword("toFixed", 7);
        lex.advance();
        if (!expect(Tok::LParen, "expected '(' after the conversion")) return 0;
        VReg v = parseExpr();
        if (failed) return 0;
        if (up && exprIsFixed) { fail("this value is already fixed"); return 0; }
        if (!up && !exprIsFixed) { fail("this value is already a whole number"); return 0; }
        // A literal is range-checked here, since `toFixed(40000)` would shift past Q16.16.
        if (up && exprLitConst >= 0) {
            const int32_t lit = ir.ops[exprLitConst].imm;
            if (lit < -32768 || lit > 32767)
                { fail("this number is out of range for a fixed value"); return 0; }
        }
        if (!expect(Tok::RParen, "expected ')' to close the conversion")) return 0;
        VReg dst = alloc();
        emit({up ? IrOp::Shl : IrOp::Sar, dst, v, 0,0,0, 16, nullptr, {}});
        freeTemp(v);
        exprIsFixed = up;
        return dst;
    }

    /// Parse what a name reads as a value: a system variable, a struct field, a local, a member, or a call's answer.
    VReg parseNamePrimary() {
        // Resolved before locals and members, so a system variable means one thing everywhere.
        if (const SysVar* sv = sysvars.find(lex.identBeg, lex.identLen)) {
            lex.advance();
            if (sv->kind == SysVarKind::Arg) {
                VReg v = alloc();   // parked at entry, so bring it back for this read
                emit({IrOp::Reload, v, 0,0,0,0, hostArgSlot(sv->where), nullptr, {}});
                return v;
            }
            VReg v = alloc();
            // A 4-byte load, since a one-byte read gave a 768-wide wall a width of zero.
            emit({IrOp::LoadCtrl32, v, 0,0,0,0, sv->where, nullptr, {}});
            return v;
        }
        // A whole struct in a scalar place is read by field: `getRGB(i).r`, `RGB(1, 2, 3).g`.
        if (const StructDef* st = structCallAt()) {
            VReg vals[kMaxStructFields] = {};
            if (!parseStructValue(*st, vals, /*thenField*/ true)) return 0;
            const int f = parseField(*st);
            for (uint8_t k = 0; k < st->fieldCount; k++) if (int(k) != f) freeTemp(vals[k]);
            if (f < 0) return 0;
            exprIsFixed = false;
            exprLitConst = -1;
            return vals[f];
        }
        const int li = findLocal(lex.identBeg, lex.identLen);
        if (li >= 0) {
            // Into an ordinary temp, so a variable holds a register only for this read.
            lex.advance();
            // A struct local's field sits in the slot after the one before it.
            const StructDef* lst = structAt(locals[li].st);
            const int f = lst ? parseField(*lst) : 0;
            if (f < 0) return 0;
            VReg v = alloc();
            emit({IrOp::Reload, v, 0,0,0,0, locals[li].slot + f, nullptr, {}});
            exprIsFixed = (locals[li].type == CtrlType::Fixed);
            return v;
        }
        // One lookup answers both, since a control is a member the UI shows.
        const int mi = findMember(lex.identBeg, lex.identLen);
        if (mi >= 0) return parseMemberRead(mi);
        VReg out = 0;
        const Builtin* called = table.find(lex.identBeg, lex.identLen);
        parseCall(&out);   // otherwise a call used as an expression must return a value
        // The builtin's business: uvX and uvY hand back a fixed coordinate, and min follows its arguments.
        exprIsFixed = called && (called->fixedReturn || (called->sameTypeArgs && argsFixed));
        exprLitConst = -1;
        return out;
    }

    /// Read member `mi`, the cursor past its name: a struct field, an element, or the whole value.
    VReg parseMemberRead(int mi) {
        lex.advance();
        if (const StructDef* mst = structAt(memberStruct[mi])) {
            const int f = parseField(*mst);
            if (f < 0) return 0;
            exprIsFixed = false;
            return loadMemberField(mi, f);
        }
        // An arbitrary index expression, the orthogonality the rest of the language has.
        if (lex.kind == Tok::LBracket) {
            if (members[mi].count == 1) { fail("this member is not an array"); return 0; }
            lex.advance();
            VReg idx = parseExpr();
            if (failed) return 0;
            // An index counts elements, so it is a whole number.
            if (exprIsFixed) { fail("an array index is a whole number: write toInt(x)"); return 0; }
            if (!expect(Tok::RBracket, "expected ']' to close an array index")) { freeTemp(idx); return 0; }
            VReg v = alloc();
            // The element's type, since leaving the index's made `heat[3] * 0.5` patch the 3.
            exprIsFixed = (members[mi].type == CtrlType::Fixed);
            exprLitConst = -1;
            emit({IrOp::LoadIdx, v, idx, 0, 0, 0,
                  idxPack(members[mi].offset, ctrlWidth(members[mi].type),
                          members[mi].count), nullptr, {}});
            freeTemp(idx);
            return v;
        }
        if (members[mi].count > 1) { fail("an array needs an index: write name[i]"); return 0; }
        VReg v = alloc();
        exprIsFixed = (members[mi].type == CtrlType::Fixed);
        // One load for every scalar, since a slot already holds what its type promises.
        emit({IrOp::LoadCtrl32, v, 0,0,0,0, members[mi].offset, nullptr, {}});
        return v;
    }

    // A bare ident reads a declared value, where one followed by `(` is a call.
    /// Parse a primary: a literal, a name, a call, a conversion or a parenthesized expression.
    VReg parsePrimary() {
        if (failed) return 0;
        // Set here, not per branch, so a new kind of primary cannot forget either question.
        exprIsFixed = false;
        exprLitConst = -1;
        if (lex.kind == Tok::Ident && (atKeyword("toFixed", 7) || atKeyword("toInt", 5))) return parseConversion();
        if (lex.kind == Tok::LParen) {                   // grouping
            lex.advance();
            VReg v = parseExpr();
            if (!expect(Tok::RParen, "expected ')'")) return 0;
            return v;
        }
        if (lex.kind == Tok::Minus) {                    // unary minus: 0 - v, as (v * -1)
            lex.advance();
            // Folded before the positive form is checked, since INT32_MIN needs its sign attached.
            if (lex.kind == Tok::Number) {
                const int64_t neg = -lex.number;
                if (neg < INT32_MIN || neg > INT32_MAX) { fail("number out of range"); return 0; }
                VReg v = alloc();
                emit({IrOp::Const, v, 0,0,0,0, static_cast<int32_t>(neg), nullptr, {}});
                // A fixed literal folds too, since -32768.0 is one past the positive limit.
                exprIsFixed = lex.numberIsFixed;
                exprLitConst = lex.numberIsFixed ? -1 : int(ir.count) - 1;
                lex.advance();
                return v;
            }
            VReg v = parsePrimary();
            if (failed) return 0;
            VReg m = alloc();
            emit({IrOp::Const, m, 0,0,0,0, -1, nullptr, {}});
            VReg dst = alloc();
            emit({IrOp::Mul, dst, v, m, 0,0, 0, nullptr, {}});
            freeTemp(m); freeTemp(v);
            return dst;
        }
        // Ordinary literals evaluating to 1 and 0, so every arithmetic path takes them unchanged.
        if (lex.kind == Tok::Ident && (atKeyword("true", 4) || atKeyword("false", 5))) {
            VReg v = alloc();
            emit({IrOp::Const, v, 0,0,0,0, atKeyword("true", 4) ? 1 : 0, nullptr, {}});
            lex.advance();
            return v;
        }
        if (lex.kind == Tok::Number) {
            // The whole signed range, since a member holds 32 bits and a literal must reach it.
            if (lex.number < INT32_MIN || lex.number > INT32_MAX)
                { fail("number out of range"); return 0; }
            // A positive fixed literal stops at 32767.99998, since 32768.0 needs the minus.
            if (lex.numberIsFixed && lex.number > INT32_MAX)      // 32767.99998 in Q16.16
                { fail("number out of range for a fixed value"); return 0; }
            VReg v = alloc();
            emit({IrOp::Const, v, 0,0,0,0, static_cast<int32_t>(lex.number), nullptr, {}});
            // A decimal point made it a fixed value at the lexer; the word is already scaled.
            exprIsFixed = lex.numberIsFixed;
            // Recorded so a meet point can patch this op when it adopts fixed.
            if (!lex.numberIsFixed) exprLitConst = int(ir.count) - 1;
            lex.advance();
            return v;
        }
        if (lex.kind == Tok::Ident) return parseNamePrimary();
        fail(lex.kind == Tok::Not ? kJoinsConditions : "expected a number, a control name, or a function call");
        return 0;
    }

    /// Parse a call; with `resultOut` it is an expression and must return a value.
    void parseCall(VReg* resultOut) {
        if (lex.kind != Tok::Ident) { fail("expected a function name"); return; }
        const Builtin* fn = table.find(lex.identBeg, lex.identLen);
        if (!fn) {
            // Against the class list, and only functions already parsed, though one sees itself.
            for (uint8_t i = 0; i < fnCount; i++) {
                if (fns[i].nameLen != lex.identLen) continue;
                if (std::strncmp(fns[i].name, lex.identBeg, lex.identLen) != 0) continue;
                lex.advance();
                if (!expect(Tok::LParen, "expected '(' after the function name")) return;
                if (!stageScriptArgs(i)) return;
                // Refused for a void callee, since the script would read the register's leftovers.
                if (resultOut) {
                    if (fns[i].ret == RetType::Void) {
                        fail("this function returns nothing, so it cannot be used as a value"); return;
                    }
                    if (fns[i].ret == RetType::Str) {
                        fail("a string return cannot be used in an expression yet"); return;
                    }
                    // A script call preserves the caller's vregs, or `a() + b()` reads one twice.
                    const VReg v = alloc();
                    emit({IrOp::CallScript, v, 0, 1, 0, 0, static_cast<int32_t>(i), nullptr, {}});
                    *resultOut = v;
                    return;
                }
                // A function number rather than an op index, which the spill pass would shift.
                emit({IrOp::CallScript, 0, 0,0,0,0, static_cast<int32_t>(i), nullptr, {}});
                return;
            }
            fail("unknown function"); return;
        }
        lex.advance();
        if (!expect(Tok::LParen, "expected '(' after the function name")) return;

        // Parked in consecutive frame slots, so only one argument holds a register at a time.
        const uint8_t argBase = slotHighWater;
        uint8_t n = 0;
        if (!parseBuiltinArgs(fn, n)) return;
        // A builtin answering a struct writes it into its first argument slots, so they stay reserved.
        if (fn->retStruct) {
            const StructDef* st = table.findStruct(fn->retStruct, std::strlen(fn->retStruct));
            if (st && !reserveStructAnswer(argBase, *st)) return;
        }

        // The arguments live in the frame, so nothing is held in a register across the call.
        if (resultOut) {
            if (fn->kind != BuiltinKind::Call || !fn->returns) { fail("this function does not return a value"); return; }
            // `imm` carries the slot they start at and `b` how many there are.
            VReg r = alloc();
            emit({IrOp::Call, r, 0, n, 0, 0, argBase, fn->fn, {}});
            *resultOut = r;
        } else {
            // A statement call, whose result is discarded when it has one.
            if (fn->kind == BuiltinKind::Call) {
                VReg r = alloc();
                emit({IrOp::Call, r, 0, n, 0, 0, argBase, fn->fn, {}});
                freeTemp(r);
            } else {
                // Reloaded into registers, and a wider builtin is refused rather than truncated.
                if (n > 4) { fail("this function takes too many arguments to inline"); return; }
                VReg a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                VReg* slot[4] = {&a0, &a1, &a2, &a3};
                for (uint8_t i = 0; i < n && i < 4; i++) {
                    *slot[i] = alloc();
                    emit({IrOp::Reload, *slot[i], 0,0,0,0, static_cast<int32_t>(argBase + i), nullptr, {}});
                }
                emit({IrOp::Inline, 0, a0, a1, a2, a3, 0, nullptr, nullptr, fn->inlineOp});
                for (uint8_t i = 0; i < n && i < 4; i++) freeTemp(*slot[i]);
            }
        }
        // Back only to where this call started, since a nested one stages inside its parent's.
        slotHighWater = argBase;
    }

    // Parked in consecutive frame slots, so only one argument holds a register at a time.
    /// Parse a builtin's arguments through its ')' into frame slots from the high-water mark, a struct expanding into its fields; `n` answers how many.
    bool parseBuiltinArgs(const Builtin* fn, uint8_t& n) {
        n = 0;
        // Local, since an argument may itself be a same-type call that would overwrite the member mid-list.
        bool groupFixed = false;
        int groupLit = -1;
        if (lex.kind != Tok::RParen) {
            while (true) {
                if (n >= fn->argc) { fail("too many arguments"); return false; }
                // A whole struct fills as many argument positions as it has fields.
                const StructDef* st = (lex.kind == Tok::Ident && !((fn->byRef >> n) & 1u)) ? structValueAt() : nullptr;
                if (st) {
                    if (n + st->fieldCount > fn->argc) { fail("too many arguments: this struct fills more positions than remain"); return false; }
                    for (uint8_t f = 0; f < st->fieldCount; f++) {
                        const uint8_t bit = static_cast<uint8_t>(1u << (n + f));
                        if ((fn->fixedArgs | fn->byStr | fn->byRef) & bit) {
                            fail("a struct fills whole-number positions only, and this one is not"); return false;
                        }
                    }
                    VReg vals[kMaxStructFields] = {};
                    if (!parseStructValue(*st, vals)) return false;
                    if (slotHighWater + st->fieldCount > kMaxLocals) { fail("too many arguments to hold"); return false; }
                    for (uint8_t f = 0; f < st->fieldCount; f++) {
                        emit({IrOp::Spill, 0, vals[f], 0,0,0, slotHighWater++, nullptr, {}});
                        freeTemp(vals[f]);
                    }
                    n = static_cast<uint8_t>(n + st->fieldCount);
                    if (lex.kind == Tok::Comma) { lex.advance(); continue; }
                    break;
                }
                // A string label and a member by name, only where the builtin asks for them.
                VReg v = 0;
                const bool wantStr = (fn->byStr >> n) & 1u;
                if (wantStr && lex.kind != Tok::String) {
                    fail("this argument must be a name in quotes"); return false;
                }
                // Only where wanted, or a pointer's low bits would arrive as a color index.
                if (!wantStr && lex.kind == Tok::String) {
                    fail("this argument is a number, not a name in quotes"); return false;
                }
                if (lex.kind == Tok::String) {
                    // Interned into a pool outliving the compile, not a pointer into the source.
                    const char* interned = internString(lex.identBeg, lex.identLen);
                    if (!interned) { fail("no room for this script's strings"); return false; }
                    v = alloc();
                    emit({IrOp::ConstPtr, v, 0,0,0,0, 0, nullptr, interned, {}});
                    lex.advance();
                } else if ((fn->byRef >> n) & 1u) {
                    if (lex.kind != Tok::Ident) { fail("expected the member this control is bound to"); return false; }
                    const int mi = findMember(lex.identBeg, lex.identLen);
                    if (mi < 0) { fail("no member of that name is declared in this class"); return false; }
                    // The type must have a widget, and drive one value rather than an array.
                    if (members[mi].type == CtrlType::Fixed || members[mi].type == CtrlType::Str)
                        { fail("a control binds an int, byte or bool member"); return false; }
                    if (members[mi].count > 1)
                        { fail("a control binds a single member, not an array"); return false; }
                    // Offset and type in one word, since the builtin sees values, not declarations.
                    v = alloc();
                    emit({IrOp::Const, v, 0,0,0,0,
                          members[mi].offset | (int32_t(members[mi].type) << 8), nullptr, {}});
                    lex.advance();
                } else if (fn->sameTypeArgs) {
                    // One type across the arguments, a literal adopting it, and the result following it.
                    v = parseExpr();
                    if (failed) return false;
                    if (n == 0) {
                        groupFixed = exprIsFixed;
                        groupLit = exprLitConst;
                    } else {
                        bool outFixed = false;
                        if (!meet(groupFixed, groupLit, exprIsFixed, exprLitConst, outFixed)) return false;
                        groupFixed = outFixed;
                        groupLit = -1;
                    }
                } else {
                    const bool wantFixed = ((fn->fixedArgs >> n) & 1u) != 0;
                    v = parseExpr();
                    if (failed) return false;
                    // Against what the builtin declares, since a stray fixed reads 65,536 times off.
                    if (wantFixed != exprIsFixed) {
                        bool adopted = false;
                        if (!wantFixed || !meet(true, -1, exprIsFixed, exprLitConst, adopted)) {
                            fail(wantFixed ? "this argument is a fixed value: write toFixed(x)"
                                           : "this argument is a whole number: write toInt(x)");
                            return false;
                        }
                    }
                }
                if (failed) return false;
                if (slotHighWater >= kMaxLocals) { fail("too many arguments to hold"); return false; }
                emit({IrOp::Spill, 0, v, 0,0,0, slotHighWater++, nullptr, {}});
                freeTemp(v);                       // its register is free again at once
                n++;
                if (lex.kind == Tok::Comma) { lex.advance(); continue; }
                break;
            }
        }
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        if (n != fn->argc) { fail("wrong number of arguments"); return false; }
        argsFixed = groupFixed;   // read by the caller straight after, before anything else parses
        return expect(Tok::RParen, "expected ')'");
    }

    // Its fields pack at their own width from a 4-byte boundary, as an array's elements do, and start at zero.
    /// Parse a struct member, `RGB c;`, the type name already consumed.
    void parseStructDecl(const StructDef& st) {
        if (lex.kind != Tok::Ident) { fail("expected a member name after the type"); return; }
        const char* name = lex.identBeg; size_t nameLen = lex.identLen;
        if (nameLen >= kMaxControlName) { fail("member name too long"); return; }
        if (sysvars.find(name, nameLen)) { fail("name is a system variable"); return; }
        if (findMember(name, nameLen) >= 0) { fail("duplicate member name"); return; }
        if (table.find(name, nameLen)) { fail("member name shadows a built-in function"); return; }
        if (isReservedWord(name, nameLen)) { fail("member name is a reserved word"); return; }
        if (table.findStruct(name, nameLen)) { fail("that name is a type"); return; }
        lex.advance();
        if (lex.kind == Tok::Assign) { fail("a struct member starts at zero: set its fields in a function"); return; }
        if (!expect(Tok::Semicolon, "expected ';' after the member declaration")) return;
        if (memberCount >= kMaxCtrls) { fail("too many members"); return; }
        uint16_t at = memberBytes;
        if ((at % 4) != 0) at = uint16_t(at + (4 - at % 4));
        const uint16_t need = uint16_t(st.fieldCount) * ctrlWidth(st.type);
        if (at + need > kCtrlBytes) { fail("the class declares more member data than the arena holds"); return; }
        members[memberCount] = {name, 0, 0, 0, static_cast<uint8_t>(nameLen), st.type,
                                static_cast<uint8_t>(at), st.fieldCount};
        memberStruct[memberCount] = structIndex(&st);
        memberBytes = static_cast<uint8_t>(at + need);
        memberCount++;
    }

    // The leading type keyword is already consumed by the caller.
    /// Parse a member declaration, recording it; the UI shows it only if defineControls names it.
    void parseDecl(CtrlType type) {
        if (lex.kind != Tok::Ident) { fail("expected a member name after the type"); return; }
        const char* name = lex.identBeg; size_t nameLen = lex.identLen;
        if (nameLen >= kMaxControlName) { fail("member name too long"); return; }   // never truncate
        if (sysvars.find(name, nameLen)) { fail("name is a system variable"); return; }
        // Against members, or two of one name would both exist and the second be unreachable.
        if (findMember(name, nameLen) >= 0) { fail("duplicate member name"); return; }
        // A member must not shadow a builtin, which would make the call ambiguous.
        if (table.find(name, nameLen)) { fail("member name shadows a built-in function"); return; }
        // A reserved word resolves first, so such a member could be declared and never read.
        if (isReservedWord(name, nameLen)) { fail("member name is a reserved word"); return; }
        if (table.findStruct(name, nameLen)) { fail("that name is a type"); return; }
        lex.advance();
        // The length is a literal, since the arena is sized at compile time.
        uint8_t count = 1;
        if (lex.kind == Tok::LBracket) {
            // A string is a reference into the pool, and there is no runtime string to fill one.
            if (type == CtrlType::Str) { fail("string arrays are not supported"); return; }
            // A fixed array waits for per-element type tracking, refused rather than half-working.
            if (type == CtrlType::Fixed) { fail("fixed arrays are not supported yet"); return; }
            lex.advance();
            if (lex.kind != Tok::Number) { fail("expected an array length (a number)"); return; }
            if (lex.number < 1 || lex.number > kCtrlBytes) { fail("array length out of range"); return; }
            count = static_cast<uint8_t>(lex.number);
            lex.advance();
            if (!expect(Tok::RBracket, "expected ']' to close the array length")) return;
            if (!expect(Tok::Semicolon, "expected ';': an array has no initializer")) return;
            if (lex.kind == Tok::Error) { fail(lex.err); return; }
            if (memberCount >= kMaxCtrls) { fail("too many members"); return; }
            // Elements pack at their own width, but the array starts 4-byte aligned.
            const uint8_t elem = ctrlWidth(type);
            uint16_t at = memberBytes;
            if ((at % 4) != 0) at = uint16_t(at + (4 - at % 4));
            const uint16_t need = uint16_t(count) * elem;
            if (at + need > kCtrlBytes) { fail("the class declares more member data than the arena holds"); return; }
            // Zeroed, since the range arrives with addControl rather than the declaration.
            members[memberCount] = {name, 0, 0, 0, static_cast<uint8_t>(nameLen), type,
                                    static_cast<uint8_t>(at), count};
            memberBytes = static_cast<uint8_t>(at + need);
            memberCount++;
            return;
        }
        if (!expect(Tok::Assign, "expected '=' in a member declaration")) return;
        // A leading minus only here, where a number is the only thing that can follow.
        bool negated = false;
        if (lex.kind == Tok::Minus) { negated = true; lex.advance(); }
        // true and false seed a bool the way a script writes one.
        if (lex.kind == Tok::Ident && (atKeyword("true", 4) || atKeyword("false", 5))) {
            if (type != CtrlType::Bool) { fail("true and false initialize a bool member"); return; }
            // A sign has no meaning on a boolean, and `-true` seeded 1 as though unwritten.
            if (negated) { fail("true and false take no sign"); return; }
            const long b = atKeyword("true", 4) ? 1 : 0;
            lex.advance();
            if (!expect(Tok::Semicolon, "expected ';' after the member declaration")) return;
            if (lex.kind == Tok::Error) { fail(lex.err); return; }
            if (memberCount >= kMaxCtrls) { fail("too many members"); return; }
            uint16_t bat = memberBytes;
            if ((bat % 4) != 0) bat = uint16_t(bat + (4 - bat % 4));
            if (bat + 4 > kCtrlBytes)
                { fail("the class declares more member data than the arena holds"); return; }
            members[memberCount] = {name, 0, 1, static_cast<int32_t>(b),
                                    static_cast<uint8_t>(nameLen), type,
                                    static_cast<uint8_t>(bat), 1};
            memberBytes = static_cast<uint8_t>(bat + 4);
            memberCount++;
            return;
        }
        // Only a string member reaches here quoted, so say that rather than asking for a number.
        if (lex.kind == Tok::String) { fail("a string member cannot be initialized yet"); return; }
        if (lex.kind != Tok::Number) { fail("expected a default value (a number)"); return; }
        if (negated) lex.number = -lex.number;
        // Against the declared type, so `byte n = 300;` is refused rather than becoming 44.
        int64_t defMin = INT32_MIN, defMax = INT32_MAX;
        const char* rangeErr = nullptr;
        switch (type) {
            case CtrlType::Byte:  defMin = 0; defMax = 255;
                                  rangeErr = "byte default out of range (0..255)";
                                  if (lex.numberIsFixed)
                                      { fail("a byte member takes a whole number"); return; }
                                  break;
            case CtrlType::Bool:  defMin = 0; defMax = 1;
                                  rangeErr = "bool default is 0 or 1";
                                  if (lex.numberIsFixed)
                                      { fail("a bool member takes true or false"); return; }
                                  break;
            case CtrlType::Str:   fail("a string member cannot be initialized yet"); return;
            case CtrlType::Int:   rangeErr = "default out of range";
                                  if (lex.numberIsFixed)
                                      { fail("an int member takes a whole number"); return; }
                                  break;
            case CtrlType::Fixed:
                // Converted at compile time, since `fixed zoom = 2;` means 2.0 and costs no shift.
                if (!lex.numberIsFixed) {
                    if (lex.number < -32768 || lex.number > 32767)
                        { fail("fixed default out of range (-32768.0..32767.99998)"); return; }
                    lex.number = lex.number << 16;
                }
                rangeErr = "fixed default out of range (-32768.0..32767.99998)";
                break;
        }
        if (lex.number < defMin || lex.number > defMax) { fail(rangeErr); return; }
        int64_t def = lex.number;
        lex.advance();
        if (!expect(Tok::Semicolon, "expected ';' after the member declaration")) return;
        // Surface the lexer's own message rather than a generic later failure.
        if (lex.kind == Tok::Error) { fail(lex.err); return; }
        // The range belongs to the control, so a member no control surfaces has none.
        if (memberCount >= kMaxCtrls) { fail("too many members"); return; }
        // A running byte cursor, since every scalar takes one 4-byte slot whatever its type.
        uint16_t at = memberBytes;
        if ((at % 4) != 0) at = uint16_t(at + (4 - at % 4));
        const uint16_t need = ctrlSlotBytes(type);
        if (at + need > kCtrlBytes) { fail("the class declares more member data than the arena holds"); return; }
        // def stays int32_t, since narrowing truncated `uint16_t phase = 1000;` to 232.
        members[memberCount] = {name, 0, 0, static_cast<int32_t>(def),
                                static_cast<uint8_t>(nameLen), type,
                                static_cast<uint8_t>(at), 1};
        memberBytes = static_cast<uint8_t>(at + need);
        memberCount++;
    }

    /// Whether a name is one the expression parser resolves before it looks for a member.
    static bool isReservedWord(const char* n, size_t len) {
        static const struct { const char* w; size_t len; } kWords[] = {
            {"toFixed", 7}, {"toInt", 5}, {"true", 4}, {"false", 5},
            // The type keywords too, since `int int = 5;` parsed and confused the class loop.
            {"int", 3}, {"byte", 4}, {"bool", 4}, {"fixed", 5}, {"string", 6},
            // And the statement keywords, since `int if = 0;` made every later `if` a reference.
            {"if", 2}, {"else", 4}, {"for", 3}, {"return", 6}, {"class", 5}, {"void", 4}};
        for (const auto& k : kWords)
            if (len == k.len && std::strncmp(n, k.w, k.len) == 0) return true;
        return false;
    }

    /// The struct type the current token names, or null.
    const StructDef* structTypeAt() const {
        return lex.kind == Tok::Ident ? table.findStruct(lex.identBeg, lex.identLen) : nullptr;
    }
    /// Whether a struct declaration starts here: a type name followed by a variable name.
    bool structDeclAt() const {
        if (!structTypeAt()) return false;
        Lexer probe = lex;
        probe.advance();
        return probe.kind == Tok::Ident;
    }

    // Matched by text rather than lexed, since the set is tiny and a length check is exact.
    /// Whether the current token is exactly this keyword.
    bool atKeyword(const char* kw, size_t len) const {
        return lex.kind == Tok::Ident && lex.identLen == len && std::strncmp(lex.identBeg, kw, len) == 0;
    }
    // Names an author reaches for rather than storage widths, which are the compiler's business.
    /// Whether the current token opens a type: int, byte, bool, fixed or string.
    bool atTypeKeyword() const {
        return atKeyword("int", 3) || atKeyword("byte", 4) || atKeyword("bool", 4) ||
               atKeyword("fixed", 5) || atKeyword("string", 6);
    }
    // Not every member type, since `byte tick()` would suggest a narrowing the engine never does.
    /// Whether the current token opens a return type: void, int or string.
    bool atRetKeyword() const {
        return atKeyword("void", 4) || atKeyword("int", 3) || atKeyword("string", 6);
    }
    /// The return type the current token names.
    RetType currentRet() const {
        if (atKeyword("int", 3))    return RetType::Int;
        if (atKeyword("string", 6)) return RetType::Str;
        return RetType::Void;
    }
    /// The type the current keyword names. Only called when atTypeKeyword() is true.
    CtrlType currentType() const {
        if (atKeyword("byte", 4))   return CtrlType::Byte;
        if (atKeyword("bool", 4))   return CtrlType::Bool;
        if (atKeyword("fixed", 5))  return CtrlType::Fixed;
        if (atKeyword("string", 6)) return CtrlType::Str;
        return CtrlType::Int;
    }

    // Bottom-tested, and the limit is re-tested at the top so an overshooting step terminates.
    /// Parse a `for` loop and emit it.
    bool parseFor() {
        lex.advance();                                     // consume `for`
        if (!expect(Tok::LParen, "expected '(' after for")) return false;

        // The counter is declared like every other variable, so none appears out of nowhere.
        if (!atKeyword("int", 3)) {
            fail("a loop counter is declared: for (int i = 0; ...)");
            return false;
        }
        lex.advance();                                     // consume `int`
        if (lex.kind != Tok::Ident) { fail("expected a loop variable"); return false; }
        // Two slots per loop, both outliving the body, so nesting is bounded by frame slots.
        if (localCount + 2 > kMaxLocals) { fail("too many nested loops"); return false; }
        const char* varName = lex.identBeg;
        const size_t varLen = lex.identLen;
        if (sysvars.find(varName, varLen)) { fail("name is a system variable"); return false; }
        // A reused name would let the inner step write what the outer back edge tests, and hang.
        if (findLocal(varName, varLen) >= 0) { fail("loop variable already in use"); return false; }
        lex.advance();
        if (!expect(Tok::Assign, "expected '=' in the for's first clause")) return false;
        VReg init = parseExpr();
        // A loop counts, so a fixed limit would run the body 65,536 times.
        if (exprIsFixed) { fail("a loop counts in whole numbers: write toInt(x)"); return false; }
        if (failed) return false;
        // Bounded on slotHighWater too, since a call releases staging slots it never counted.
        if (slotHighWater >= kMaxLocals) { fail("too many loop variables"); return false; }
        const uint8_t counterSlot = slotHighWater++;
        emit({IrOp::Spill, 0, init, 0,0,0, counterSlot, nullptr, {}});
        freeTemp(init);
        const uint8_t myLocal = localCount;
        locals[localCount++] = {varName, varLen, counterSlot, CtrlType::Int};   // a counter is whole
        if (!expect(Tok::Semicolon, "expected ';' after the for's first clause")) return false;

        // The name must be the loop variable, since the emitted code tests the counter regardless.
        if (lex.kind != Tok::Ident) { fail("expected the loop variable in the condition"); return false; }
        if (lex.identLen != varLen || std::strncmp(lex.identBeg, varName, varLen) != 0) {
            fail("the condition must test the loop variable"); return false;
        }
        lex.advance();
        // `<=` is a limit one higher, and a descending loop is refused as unmodelled.
        bool inclusive = false;
        if (lex.kind == Tok::LessEq) { inclusive = true; lex.advance(); }
        else if (lex.kind == Tok::Greater || lex.kind == Tok::GreaterEq) {
            fail("a for counts up: use `<` or `<=` and invert the index inside");
            return false;
        }
        else if (!expect(Tok::Less, "expected '<' or '<=' in the for condition")) return false;
        // Its own slot, since the bound is read at the guard and the back edge and outlives a call.
        VReg limitTmp = parseExpr();
        if (exprIsFixed) { fail("a loop counts in whole numbers: write toInt(x)"); return false; }
        if (failed) return false;
        if (inclusive) {
            // `i <= n` is `i < n + 1`, leaving the guard untouched.
            VReg one = alloc();
            emit({IrOp::Const, one, 0,0,0,0, 1, nullptr, {}});
            emit({IrOp::Add, limitTmp, limitTmp, one, 0,0, 0, nullptr, {}});
            freeTemp(one);
        }
        if (slotHighWater >= kMaxLocals) { fail("too many loop variables"); return false; }
        const uint8_t limitSlot = slotHighWater++;
        emit({IrOp::Spill, 0, limitTmp, 0,0,0, limitSlot, nullptr, {}});
        freeTemp(limitTmp);
        if (!expect(Tok::Semicolon, "expected ';' after the for's condition")) return false;

        // --- step: ident = expr (parsed now, emitted after the body) ---
        if (lex.kind != Tok::Ident) { fail("expected the loop variable in the step"); return false; }
        if (lex.identLen != varLen || std::strncmp(lex.identBeg, varName, varLen) != 0) {
            fail("the step must advance the loop variable"); return false;   // it advances the counter
        }
        lex.advance();
        if (!expect(Tok::Assign, "expected '=' in the for's third clause")) return false;
        const char* stepSrc = lex.tokBeg;                  // re-lexed after the body
        // Skip the step expression without emitting: scan to the closing ')'.
        int depth = 0;
        while (!failed && lex.kind != Tok::End) {
            // A lexer error stops the scan, since advance() would not move past the character.
            if (lex.kind == Tok::Error) { fail(lex.err); return false; }
            if (lex.kind == Tok::LParen) depth++;
            else if (lex.kind == Tok::RParen) { if (depth == 0) break; depth--; }
            lex.advance();
        }
        if (!expect(Tok::RParen, "expected ')' to close the for")) return false;
        if (!expect(Tok::LBrace, "expected '{': a for's body is braced")) return false;

        if (nextLabel + 2 > kIrLabels) { fail("too many loops in one script"); return false; }
        const uint8_t lDone = nextLabel++;
        const uint8_t lTop  = nextLabel++;

        // Both operands reload from the frame, so the body's demand is independent of nesting.
        {
            VReg c = alloc(), l = alloc();
            emit({IrOp::Reload, c, 0,0,0,0, counterSlot, nullptr, {}});
            emit({IrOp::Reload, l, 0,0,0,0, limitSlot,   nullptr, {}});
            emit({IrOp::BranchGe, 0, c, l, 0,0, lDone, nullptr, {}});   // empty range
            freeTemp(l); freeTemp(c);
        }
        emit({IrOp::Label,    0, 0,0,0,0,             lTop,  nullptr, {}});

        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End) {
            if (!parseStatement()) return false;
        }
        if (!expect(Tok::RBrace, "expected '}' to close the for's body")) return false;

        // The step, re-lexed from the source it was skipped over.
        {
            Lexer stepLex(stepSrc);
            Lexer save = lex;
            lex = stepLex;
            VReg s = parseExpr();
            if (exprIsFixed) { fail("a loop counts in whole numbers: write toInt(x)"); return false; }
            if (failed) return false;
            // parseExpr stops at what it cannot consume, so `i = i + 1 garbage` compiled clean.
            if (lex.kind != Tok::RParen) {
                lex = save; fail("unexpected token in the for's step"); return false;
            }
            emit({IrOp::Spill, 0, s, 0,0,0, counterSlot, nullptr, {}});
            freeTemp(s);
            lex = save;
        }
        // Re-tested at the top, since a step jumping past the limit never reaches equality.
        {
            VReg c = alloc(), l = alloc();
            emit({IrOp::Reload, c, 0,0,0,0, counterSlot, nullptr, {}});
            emit({IrOp::Reload, l, 0,0,0,0, limitSlot,   nullptr, {}});
            emit({IrOp::BranchGe, 0, c, l, 0,0, lDone, nullptr, {}});
            emit({IrOp::BranchNe, 0, c, l, 0,0, lTop,  nullptr, {}});
            freeTemp(l); freeTemp(c);
        }
        emit({IrOp::Label,    0, 0,0,0,0,             lDone, nullptr, {}});

        localCount = myLocal;                              // the loop variable leaves scope
        // Its two slots return, so sequential loops reuse the space while nesting still stacks.
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        slotHighWater = counterSlot;
        return true;
    }

    // A system variable may not be assigned, since the engine rewrites it before every call.
    /// Parse an assignment, with the name already consumed.
    bool parseAssignment(const char* name, size_t nameLen) {
        if (lex.kind == Tok::Dot) return parseFieldAssignment(name, nameLen);
        // Resolved before the '=', since an index sits between the name and the operator.
        if (lex.kind == Tok::LBracket) {
            const int ai = findMember(name, nameLen);
            if (ai < 0) { fail("no member of that name is declared in this class"); return false; }
            if (memberStruct[ai]) { fail(kOneFieldAtATime); return false; }
            if (members[ai].count == 1) { fail("this member is not an array"); return false; }
            lex.advance();
            VReg idx = parseExpr();
            if (failed) return false;
            if (exprIsFixed) { fail("an array index is a whole number: write toInt(x)"); return false; }
            if (!expect(Tok::RBracket, "expected ']' to close an array index")) { freeTemp(idx); return false; }
            if (!expect(Tok::Assign, "expected '=' in an assignment")) { freeTemp(idx); return false; }
            VReg v = parseExpr();
            if (failed) { freeTemp(idx); return false; }
            // The same wall the scalar store enforces, since the stored word carries no scaling.
            {
                const bool wantFixed = (members[ai].type == CtrlType::Fixed);
                if (wantFixed != exprIsFixed) {
                    bool adopted = false;
                    if (!wantFixed || !meet(true, -1, exprIsFixed, exprLitConst, adopted)) {
                        fail(wantFixed ? "a fixed element takes a fixed value: write toFixed(x)"
                                       : "this element takes a whole number: write toInt(x)");
                        return false;
                    }
                }
            }
            emit({IrOp::StoreIdx, 0, idx, v, 0, 0,
                  idxPack(members[ai].offset, ctrlWidth(members[ai].type),
                          members[ai].count), nullptr, {}});
            freeTemp(v); freeTemp(idx);
            return expect(Tok::Semicolon, "expected ';' after an assignment");
        }
        if (!expect(Tok::Assign, "expected '=' in an assignment")) return false;

        // The destination first, so a refused target reports the name the script wrote.
        const int li = findLocal(name, nameLen);
        const int mi = li >= 0 ? -1 : findMember(name, nameLen);
        // A whole struct copies field by field.
        if (const StructDef* st = li >= 0 ? structAt(locals[li].st) : (mi >= 0 ? structAt(memberStruct[mi]) : nullptr)) {
            VReg vals[kMaxStructFields] = {};
            if (!parseStructValue(*st, vals)) return false;
            storeStruct(*st, li, mi, vals);
            return expect(Tok::Semicolon, "expected ';' after an assignment");
        }
        if (mi >= 0 && members[mi].count > 1) {
            fail("assign one element: name[i] = value");
            return false;
        }
        if (li < 0 && mi < 0) {
            if (sysvars.find(name, nameLen)) {
                fail("a system variable is read-only");
            } else {
                fail("not declared: add it to the class body");
            }
            return false;
        }
        VReg v = parseExpr();
        if (failed) return false;
        // The wall holds at the store, since the slot holds raw bits with no scaling of its own.
        if (mi >= 0) {
            const bool wantFixed = (members[mi].type == CtrlType::Fixed);
            if (wantFixed != exprIsFixed) {
                // A literal converts at compile time; anything computed names its conversion.
                bool adopted = false;
                if (wantFixed && !meet(true, -1, exprIsFixed, exprLitConst, adopted) ) return false;
                if (!wantFixed) {
                    fail("this member takes a whole number: write toInt(x)");
                    return false;
                }
            }
        } else if ((locals[li].type == CtrlType::Fixed) != exprIsFixed) {
            // Same wall as a member, since a scaling mismatch is invisible at run time.
            bool adopted = false;
            if (locals[li].type == CtrlType::Fixed) {
                if (!meet(true, -1, exprIsFixed, exprLitConst, adopted)) return false;
            } else {
                fail("this variable takes a whole number: write toInt(x)");
                return false;
            }
        }
        if (li >= 0) narrowToType(v, locals[li].type);   // a byte local wraps at 255
        if (li >= 0) emit({IrOp::Spill,     0, v, 0,0,0, locals[li].slot,   nullptr, {}});
        // The store narrows, so a byte member truncates in the instruction itself.
        else         emit({storeOpFor(members[mi].type), 0, v, 0,0,0,
                           members[mi].offset, nullptr, {}});
        freeTemp(v);
        return expect(Tok::Semicolon, "expected ';' after an assignment");
    }

    /// Store a struct value's registers into a local's slots or a member's bytes, freeing them.
    void storeStruct(const StructDef& st, int li, int mi, VReg vals[kMaxStructFields]) {
        for (uint8_t f = 0; f < st.fieldCount; f++) {
            if (li >= 0) emit({IrOp::Spill, 0, vals[f], 0,0,0, locals[li].slot + f, nullptr, {}});
            else         emit({storeOpFor(members[mi].type), 0, vals[f], 0,0,0, memberFieldOffset(mi, f), nullptr, {}});
            freeTemp(vals[f]);
        }
    }

    /// Parse `name.field = value;`, the name consumed and the '.' current.
    bool parseFieldAssignment(const char* name, size_t nameLen) {
        const int li = findLocal(name, nameLen);
        const int mi = li >= 0 ? -1 : findMember(name, nameLen);
        const StructDef* st = li >= 0 ? structAt(locals[li].st) : (mi >= 0 ? structAt(memberStruct[mi]) : nullptr);
        if (!st) { fail(li < 0 && mi < 0 ? "not declared: add it to the class body" : "only a struct has fields"); return false; }
        const int f = parseField(*st);
        if (f < 0) return false;
        if (!expect(Tok::Assign, "expected '=' in an assignment")) return false;
        VReg v = parseExpr();
        if (failed) return false;
        if (exprIsFixed) { fail("a struct field is a whole number: write toInt(x)"); return false; }
        if (exprLitConst >= 0 && !fitsInType(ir.ops[exprLitConst].imm, st->type))
            { fail("this number is out of range for that field"); return false; }
        if (li >= 0) {
            narrowToType(v, st->type);   // a byte field wraps at 255, as a byte local does
            emit({IrOp::Spill, 0, v, 0,0,0, locals[li].slot + f, nullptr, {}});
        } else {
            emit({storeOpFor(members[mi].type), 0, v, 0,0,0, memberFieldOffset(mi, f), nullptr, {}});
        }
        freeTemp(v);
        return expect(Tok::Semicolon, "expected ';' after an assignment");
    }

    // A bool holds 0..255, so a multiple of 256 truncates to false: the IR has no select.
    /// The store op a member's type needs.
    static IrOp storeOpFor(CtrlType type) {
        return ctrlWidth(type) == 1 ? IrOp::StoreCtrl : IrOp::StoreCtrl32;
    }

    /// A fresh branch label, or -1 having failed when the script has used them all.
    int newLabel() {
        if (nextLabel >= kIrLabels) { fail("too many branches in one script"); return -1; }
        return nextLabel++;
    }

    // The jump idiom, `z >= z` with z zero, since the IR has no unconditional branch.
    /// Branch to a label always.
    void emitJump(uint8_t label) {
        VReg z = alloc();
        emit({IrOp::Const,    z, 0,0,0,0, 0,     nullptr, {}});
        emit({IrOp::BranchGe, 0, z, z, 0,0, label, nullptr, {}});
        freeTemp(z);
    }

    // Six comparisons onto two signed branch ops, each emitting the negation the script wrote.
    /// Branch to `lFalse` unless `a cmp b` holds, freeing both registers.
    void emitBranchUnless(Tok cmp, VReg a, VReg b, uint8_t lFalse) {
        switch (cmp) {
            // Signed, since an unsigned compare reads a negative as a huge value.
            case Tok::Less:      emit({IrOp::BranchGeS, 0, a, b, 0,0, lFalse, nullptr, {}}); break;
            // !(a >= b) is a < b, which needs the strict pair below.
            case Tok::GreaterEq: emitStrictLess(a, b, lFalse); break;
            // !(a > b) is a <= b, i.e. b >= a.
            case Tok::Greater:   emit({IrOp::BranchGeS, 0, b, a, 0,0, lFalse, nullptr, {}}); break;
            // !(a <= b) is a > b, i.e. b < a.
            case Tok::LessEq:    emitStrictLess(b, a, lFalse); break;
            case Tok::EqEq:      emit({IrOp::BranchNe, 0, a, b, 0,0, lFalse, nullptr, {}}); break;
            // There is no BranchEq, so this is the one case needing two branches.
            case Tok::NotEq: {
                if (nextLabel >= kIrLabels) { freeTemp(b); freeTemp(a); fail("too many branches in one script"); return; }
                const uint8_t lBody = nextLabel++;
                emit({IrOp::BranchNe, 0, a, b, 0,0, lBody, nullptr, {}});
                emitJump(lFalse);
                emit({IrOp::Label,    0, 0,0,0,0, lBody, nullptr, {}});
                break;
            }
            default: break;
        }
        freeTemp(b); freeTemp(a);
    }

    /// Whether a token compares two values.
    static bool isComparison(Tok t) {
        return t == Tok::Less || t == Tok::LessEq || t == Tok::Greater || t == Tok::GreaterEq ||
               t == Tok::EqEq || t == Tok::NotEq;
    }

    // Peeks without consuming, to the ')' that closes the condition or group the cursor is in.
    /// Whether a `||` follows at this nesting level.
    bool orFollows() const {
        Lexer probe = lex;
        int depth = 0;
        while (probe.kind != Tok::End && probe.kind != Tok::Error) {
            if (probe.kind == Tok::LParen) depth++;
            else if (probe.kind == Tok::RParen) { if (depth == 0) return false; depth--; }
            else if (probe.kind == Tok::OrOr && depth == 0) return true;
            probe.advance();
        }
        return false;
    }

    // `(a + b) < c` opens with a parenthesized expression, `(a < b) && c` with a parenthesized condition.
    /// Whether the '(' at the cursor opens a condition rather than an arithmetic group.
    bool groupIsCondition() const {
        Lexer probe = lex;
        probe.advance();                   // past the '('
        int depth = 0;
        while (probe.kind != Tok::End && probe.kind != Tok::Error) {
            if (probe.kind == Tok::LParen) depth++;
            else if (probe.kind == Tok::RParen) { if (depth == 0) return false; depth--; }
            else if (depth == 0 && (isComparison(probe.kind) || probe.kind == Tok::AndAnd ||
                                    probe.kind == Tok::OrOr || probe.kind == Tok::Not)) return true;
            probe.advance();
        }
        return false;
    }

    // Jumping code, as C compiles a condition: every test branches to `lFalse` when it fails, so `&&` needs no label and `||` one per term.
    /// Parse a condition, branching to `lFalse` when it is false and falling through when it is true.
    bool parseCondition(uint8_t lFalse) {
        if (!orFollows()) return parseConjunction(lFalse);
        const int lTrue = newLabel();
        if (lTrue < 0) return false;
        for (;;) {
            const int lNext = newLabel();
            if (lNext < 0) return false;
            if (!parseConjunction(static_cast<uint8_t>(lNext))) return false;
            emitJump(static_cast<uint8_t>(lTrue));          // this term held: the whole condition holds
            emit({IrOp::Label, 0, 0,0,0,0, lNext, nullptr, {}});
            if (!expect(Tok::OrOr, "expected '||'")) return false;
            if (!orFollows()) break;
        }
        if (!parseConjunction(lFalse)) return false;        // the last term decides
        emit({IrOp::Label, 0, 0,0,0,0, lTrue, nullptr, {}});
        return true;
    }

    /// Parse terms joined by `&&`, each branching to `lFalse` when it fails.
    bool parseConjunction(uint8_t lFalse) {
        if (!parseNegation(lFalse)) return false;
        while (!failed && lex.kind == Tok::AndAnd) {
            lex.advance();
            if (!parseNegation(lFalse)) return false;
        }
        return !failed;
    }

    /// Parse a test with any number of leading `!`, each one swapping where it branches.
    // As in C, `!` negates what follows it rather than a comparison after that, so `!a < b` would compare !a: refused, so it is read neither way.
    bool parseNegation(uint8_t lFalse) {
        if (lex.kind != Tok::Not) return parseCondPrimary(lFalse);
        lex.advance();
        if (lex.kind != Tok::Not && !(lex.kind == Tok::LParen && groupIsCondition())) {
            // A negated value holds when the value is zero.
            VReg a = parseExpr();
            if (failed) return false;
            if (isComparison(lex.kind)) { freeTemp(a); fail(kNegatesWhatFollows); return false; }
            VReg z = alloc();
            emit({IrOp::Const, z, 0,0,0,0, 0, nullptr, {}});
            emitBranchUnless(Tok::EqEq, a, z, lFalse);
            return !failed;
        }
        const int lHeld = newLabel();          // where the inner test goes when it fails, which is when this one holds
        if (lHeld < 0) return false;
        if (!parseNegation(static_cast<uint8_t>(lHeld))) return false;
        if (isComparison(lex.kind)) { fail(kNegatesWhatFollows); return false; }
        emitJump(lFalse);                      // the inner test held, so this one fails
        emit({IrOp::Label, 0, 0,0,0,0, lHeld, nullptr, {}});
        return true;
    }

    // A bare value is true when it is not zero, as C reads one, so `if (done)` and `if (!done)` read naturally.
    /// Parse one test: a parenthesized condition, a comparison, or a value.
    bool parseCondPrimary(uint8_t lFalse) {
        if (lex.kind == Tok::LParen && groupIsCondition()) {
            lex.advance();
            if (!parseCondition(lFalse)) return false;
            return expect(Tok::RParen, "expected ')' to close the condition");
        }
        VReg a = parseExpr();
        if (failed) return false;
        const Tok cmp = lex.kind;
        if (!isComparison(cmp)) {
            VReg z = alloc();
            emit({IrOp::Const, z, 0,0,0,0, 0, nullptr, {}});
            emitBranchUnless(Tok::NotEq, a, z, lFalse);
            return !failed;
        }
        const bool aFixed = exprIsFixed;
        const int  aLit   = exprLitConst;
        lex.advance();
        VReg b = parseExpr();
        if (failed) { freeTemp(a); return false; }
        // Same wall as the operators, since Q16.16 preserves order but not magnitude.
        bool cmpFixed = false;
        if (!meet(aFixed, aLit, exprIsFixed, exprLitConst, cmpFixed)) { freeTemp(b); freeTemp(a); return false; }
        emitBranchUnless(cmp, a, b, lFalse);
        return !failed;
    }

    /// Parse an `if` with an optional `else`, and emit it.
    bool parseIf() {
        lex.advance();   // `if`
        if (!expect(Tok::LParen, "expected '(' after if")) return false;
        // One label to skip the then-block, taken before the condition so a single comparison emits what it always did.
        if (nextLabel + 2 > kIrLabels) { fail("too many branches in one script"); return false; }
        const uint8_t lElse = nextLabel++;
        if (!parseCondition(lElse)) return false;
        if (!expect(Tok::RParen, "expected ')' to close the if condition")) return false;
        if (!expect(Tok::LBrace, "expected '{': an if body is braced")) return false;

        // A braced body is a scope, so a local dies at the '}' and its slot is handed back.
        const uint8_t thenLocal = localCount, thenSlot = slotHighWater;
        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End)
            if (!parseStatement()) return false;
        if (failed) return false;
        localCount = thenLocal; slotHighWater = thenSlot;
        if (!expect(Tok::RBrace, "expected '}' to close the if body")) return false;

        if (atKeyword("else", 4)) {
            lex.advance();
            if (nextLabel >= kIrLabels) { fail("too many branches in one script"); return false; }
            const uint8_t lEnd = nextLabel++;
            // The then-block jumps over the else-block, as `x >= x`, which the assemblers encode.
            VReg z = alloc();
            emit({IrOp::Const,    z, 0,0,0,0, 0,    nullptr, {}});
            emit({IrOp::BranchGe, 0, z, z, 0,0, lEnd, nullptr, {}});
            freeTemp(z);
            emit({IrOp::Label,    0, 0,0,0,0, lElse, nullptr, {}});
            // An else whose body is one if, so a chain reads as a list not a staircase.
            if (atKeyword("if", 2)) {
                const uint8_t chainLocal = localCount, chainSlot = slotHighWater;
                if (!parseIf()) return false;
                localCount = chainLocal; slotHighWater = chainSlot;
            } else {
                if (!expect(Tok::LBrace, "expected '{': an else body is braced")) return false;
                const uint8_t elseLocal = localCount, elseSlot = slotHighWater;
                while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End)
                    if (!parseStatement()) return false;
                if (failed) return false;
                localCount = elseLocal; slotHighWater = elseSlot;
                if (!expect(Tok::RBrace, "expected '}' to close the else body")) return false;
            }
            emit({IrOp::Label, 0, 0,0,0,0, lEnd, nullptr, {}});
        } else {
            emit({IrOp::Label, 0, 0,0,0,0, lElse, nullptr, {}});
        }
        return true;
    }

    // BranchGeS gives `>=` only, so the strict form branches over an unconditional jump.
    /// Branch to a label when `a` is strictly less than `b`.
    void emitStrictLess(VReg a, VReg b, uint8_t label) {
        if (nextLabel >= kIrLabels) { fail("too many branches in one script"); return; }
        const uint8_t lSkip = nextLabel++;
        emit({IrOp::BranchGeS, 0, a, b, 0,0, lSkip, nullptr, {}});  // a >= b: do NOT take the skip
        VReg z = alloc();
        emit({IrOp::Const,    z, 0,0,0,0, 0,     nullptr, {}});
        // Unsigned on purpose, since `z >= z` is the jump idiom rather than a comparison.
        emit({IrOp::BranchGe, 0, z, z, 0,0, label, nullptr, {}});   // always taken
        freeTemp(z);
        emit({IrOp::Label,    0, 0,0,0,0, lSkip, nullptr, {}});
    }

    // A member is persisted and a separate budget, so a local costs a member nothing.
    /// Parse a local declaration, which lives in a frame slot for the rest of its block.
    bool parseLocalDecl() {
        // Every value type a member has means the same here; only string is refused.
        const CtrlType declType = currentType();
        if (declType == CtrlType::Str) {
            fail("a local variable holds a number: string is a member type");
            return false;
        }
        const bool wantFixed = (declType == CtrlType::Fixed);
        lex.advance();
        if (lex.kind != Tok::Ident) { fail("expected a variable name"); return false; }
        const char* varName = lex.identBeg;
        const size_t varLen = lex.identLen;
        // The same collisions a loop counter checks for, and for the same reasons.
        if (isReservedWord(varName, varLen)) { fail("that name is a reserved word"); return false; }
        if (table.findStruct(varName, varLen)) { fail("that name is a type"); return false; }
        // A local named for a builtin would shadow it for the rest of the function.
        if (table.find(varName, varLen)) { fail("that name shadows a built-in function"); return false; }
        if (sysvars.find(varName, varLen)) { fail("name is a system variable"); return false; }
        if (findLocal(varName, varLen) >= 0) { fail("that name is already in use here"); return false; }
        if (findMember(varName, varLen) >= 0) { fail("a member of that name is declared"); return false; }
        if (localCount >= kMaxLocals || slotHighWater >= kMaxLocals) {
            fail("too many variables in this function");
            return false;
        }
        lex.advance();
        if (!expect(Tok::Assign, "a local variable is initialized: int x = 0;")) return false;
        VReg init = parseExpr();
        if (failed) return false;
        // The initializer sets the promised scaling, and a literal adopts it.
        if (wantFixed != exprIsFixed) {
            bool adopted = false;
            if (wantFixed) {
                if (!meet(true, -1, exprIsFixed, exprLitConst, adopted)) return false;
            } else {
                fail("a local variable is a whole number: write toInt(x)");
                return false;
            }
        }
        // Range-checked, so `byte b = 300;` names the mistake rather than holding 44.
        if (exprLitConst >= 0 && !fitsInType(ir.ops[exprLitConst].imm, declType)) {
            fail("this number is out of range for that type");
            return false;
        }
        const uint8_t slot = slotHighWater++;
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        narrowToType(init, declType);
        emit({IrOp::Spill, 0, init, 0,0,0, slot, nullptr, {}});
        freeTemp(init);
        locals[localCount++] = {varName, varLen, slot, declType};
        return expect(Tok::Semicolon, "expected ';' after a variable declaration");
    }

    // One slot per field for the rest of the block, from a struct value or zero.
    /// Parse a struct local, `RGB c = value;` or `RGB c;`.
    bool parseStructLocalDecl() {
        const StructDef& st = *structTypeAt();
        lex.advance();
        const char* varName = lex.identBeg;
        const size_t varLen = lex.identLen;
        if (isReservedWord(varName, varLen)) { fail("that name is a reserved word"); return false; }
        if (table.findStruct(varName, varLen)) { fail("that name is a type"); return false; }
        if (table.find(varName, varLen)) { fail("that name shadows a built-in function"); return false; }
        if (sysvars.find(varName, varLen)) { fail("name is a system variable"); return false; }
        if (findLocal(varName, varLen) >= 0) { fail("that name is already in use here"); return false; }
        if (findMember(varName, varLen) >= 0) { fail("a member of that name is declared"); return false; }
        if (localCount >= kMaxLocals || slotHighWater + st.fieldCount > kMaxLocals) {
            fail("too many variables in this function: a struct takes one slot per field");
            return false;
        }
        lex.advance();
        VReg vals[kMaxStructFields] = {};
        if (lex.kind == Tok::Semicolon) {
            for (uint8_t f = 0; f < st.fieldCount; f++) {
                vals[f] = alloc();
                emit({IrOp::Const, vals[f], 0,0,0,0, 0, nullptr, {}});
            }
        } else {
            if (!expect(Tok::Assign, "expected '=' or ';' after the variable name")) return false;
            if (!parseStructValue(st, vals)) return false;
        }
        const uint8_t slot = slotHighWater;
        slotHighWater = static_cast<uint8_t>(slotHighWater + st.fieldCount);
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        for (uint8_t f = 0; f < st.fieldCount; f++) {
            emit({IrOp::Spill, 0, vals[f], 0,0,0, slot + f, nullptr, {}});
            freeTemp(vals[f]);
        }
        locals[localCount++] = {varName, varLen, slot, st.type, structIndex(&st)};
        return expect(Tok::Semicolon, "expected ';' after a variable declaration");
    }

    /// Parse one statement: a declaration, a for, an if, a return, an assignment or a call.
    bool parseStatement() {
        if (structDeclAt()) return parseStructLocalDecl();
        if (atTypeKeyword()) return parseLocalDecl();
        if (atKeyword("for", 3)) return parseFor();
        if (atKeyword("if", 2))  return parseIf();
        if (atKeyword("return", 6)) return parseReturn();
        if (lex.kind != Tok::Ident) { fail("expected a function call or an assignment"); return false; }
        // One token of lookahead separates an assignment from a call, so the lexer rewinds.
        const char* name = lex.identBeg;
        const size_t nameLen = lex.identLen;
        Lexer save = lex;
        lex.advance();
        // `name =`, `name[` and `name.` open an assignment, where `name(` opens a call.
        if (lex.kind == Tok::Assign || lex.kind == Tok::LBracket || lex.kind == Tok::Dot) return parseAssignment(name, nameLen);
        lex = save;
        parseCall(nullptr);
        if (failed) return false;
        return expect(Tok::Semicolon, "expected ';'");
    }

    // An early exit inside tick(), and how dimensions() and tags() answer.
    /// Parse a `return`, with or without a value.
    bool parseReturn() {
        lex.advance();                       // past `return`
        if (lex.kind == Tok::Semicolon) {    // a bare return: unwind, no value
            // A bare return from a value function leaves the caller reading the register.
            if (curRet != RetType::Void) { fail("this function must return a value"); return false; }
            emit({IrOp::Ret, 0, 0,0,0,0, 0, nullptr, {}});
            lex.advance();
            return true;
        }
        // A value from a void function has nowhere to go.
        if (curRet == RetType::Void) { fail("a void function returns no value"); return false; }
        // A struct's fields go into the argument block, which the caller reads after the call.
        if (curRet == RetType::Struct) {
            VReg vals[kMaxStructFields] = {};
            if (!parseStructValue(*curRetStruct, vals)) return false;
            for (uint8_t f = 0; f < curRetStruct->fieldCount; f++) {
                emit({IrOp::StoreCtrl32, 0, vals[f], 0,0,0, scriptArgOffset(f), nullptr, {}});
                freeTemp(vals[f]);
            }
            emit({IrOp::Ret, 0, 0,0,0,0, 0, nullptr, {}});
            return expect(Tok::Semicolon, "expected ';'");
        }
        VReg v = 0;
        if (lex.kind == Tok::String) {
            if (curRet != RetType::Str) { fail("this function returns an int, not a string"); return false; }
            // A pointer, which is how tags() answers, interned so it outlives the compile.
            const char* interned = internString(lex.identBeg, lex.identLen);
            if (!interned) { fail("no room for this script's strings"); return false; }
            v = alloc();
            emit({IrOp::ConstPtr, v, 0,0,0,0, 0, nullptr, interned, {}});
            lex.advance();
        } else {
            if (curRet != RetType::Int) { fail("this function returns a string, not a number"); return false; }
            v = parseExpr();
            if (failed) return false;
        }
        // imm 1 says this return carries a value, which the spill pass reads.
        emit({IrOp::Ret, 0, v, 0,0,0, 1, nullptr, {}});
        freeTemp(v);
        return expect(Tok::Semicolon, "expected ';'");
    }

    // Parsed before anything is emitted, since a later insertion would shift every index.
    /// Parse a function's parameter list, answering how many it declares.
    bool parseParams(uint8_t* paramsOut, uint8_t* argCountOut, uint8_t argStructOut[moonlive::kMaxScriptArgs]) {
        if (!expect(Tok::LParen,  "expected '(' after the function name")) return false;
        // Cleared first, since a leftover would push the parameters up out of their slots.
        localCount = 0; slotHighWater = 0;
        uint8_t params = 0;
        while (lex.kind != Tok::RParen && lex.kind != Tok::End) {
            if (params > 0 && !expect(Tok::Comma, "expected ',' between parameters")) return false;
            // A parameter is declared like a variable, so every type means what it means elsewhere.
            const StructDef* pst = structTypeAt();
            if (!pst && !atTypeKeyword()) { fail("expected a parameter type"); return false; }
            const CtrlType t = pst ? pst->type : currentType();
            const uint8_t words = pst ? pst->fieldCount : 1;
            lex.advance();
            if (lex.kind != Tok::Ident) { fail("expected a parameter name"); return false; }
            // A parameter is a local, so a name refused inside the body is refused here.
            if (isReservedWord(lex.identBeg, lex.identLen)) {
                fail("that name is a reserved word"); return false;
            }
            if (table.findStruct(lex.identBeg, lex.identLen)) { fail("that name is a type"); return false; }
            if (table.find(lex.identBeg, lex.identLen)) {
                fail("that name shadows a built-in function"); return false;
            }
            if (sysvars.find(lex.identBeg, lex.identLen)) {
                fail("name is a system variable"); return false;
            }
            if (findLocal(lex.identBeg, lex.identLen) >= 0) {
                fail("that name is already in use here"); return false;
            }
            if (findMember(lex.identBeg, lex.identLen) >= 0) {
                fail("a member of that name is declared"); return false;
            }
            if (localCount >= kMaxLocals) { fail("too many locals"); return false; }
            // Bounded by the arena block, since a further load reads past its end.
            if (params + words > moonlive::kMaxScriptArgs) {
                fail("parameters fill at most 8 words, a struct taking one per field");
                return false;
            }
            locals[localCount++] = {lex.identBeg, lex.identLen, slotHighWater, t, structIndex(pst)};
            argStructOut[(*argCountOut)++] = structIndex(pst);   // a struct fills at least one word, so the count stays within the block
            for (uint8_t w = 0; w < words; w++) paramWordType[params + w] = t;
            slotHighWater = static_cast<uint8_t>(slotHighWater + words);
            if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
            params = static_cast<uint8_t>(params + words);
            lex.advance();
        }
        if (!expect(Tok::RParen,  "expected ')' to close the parameter list")) return false;
        if (paramsOut) *paramsOut = params;
        return true;
    }

    /// Parse a function body, whose parameters parseParams already declared as its first locals.
    bool parseFunctionBody() {
        if (!expect(Tok::LBrace,  "expected '{' to open the function body")) return false;
        // Cleared above rather than here, so the parameters stay in scope for the body.
        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End)
            if (!parseStatement()) return false;
        if (failed) return false;
        localCount = 0; slotHighWater = 0;
        return expect(Tok::RBrace, "expected '}' to close the function body");
    }

    // One top-level form, since a bare statement list would mean two parse paths forever.
    /// Parse a whole script: a class of member declarations and functions.
    bool parseProgram() {
        if (!atKeyword("class", 5)) { fail("a script is a class: expected `class <Name> { … }`"); return false; }
        lex.advance();
        if (lex.kind != Tok::Ident) { fail("expected a name after `class`"); return false; }
        // Copied, since the source is freed while the name outlives it in the status line.
        const size_t n = lex.identLen < kMaxClassName ? lex.identLen : kMaxClassName;
        std::memcpy(classNameOut, lex.identBeg, n);
        classNameOut[n] = '\0';
        lex.advance();
        if (!expect(Tok::LBrace, "expected '{' to open the class body")) return false;

        // Two tokens of lookahead, since a member and a typed function open with the same one.
        while (!failed && (atTypeKeyword() || structTypeAt())) {
            const StructDef* memberSt = structTypeAt();
            const CtrlType ty = currentType();
            Lexer save = lex;
            lex.advance();                       // past the type
            const bool isFn = lex.kind == Tok::Ident && [&] {
                Lexer probe = lex;
                probe.advance();                 // past the name
                return probe.kind == Tok::LParen;
            }();
            if (isFn) { lex = save; break; }     // a function: leave it for the loop below
            if (memberSt) parseStructDecl(*memberSt);
            else parseDecl(ty);
        }
        if (failed) return false;

        bool any = false;
        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End) {
            // Declared rather than implicit, so the host can rely on what a function hands back.
            const StructDef* retSt = structTypeAt();
            if (!atRetKeyword() && !retSt) {
                fail("a function returns void, int, string or a struct type");
                return false;
            }
            const RetType ret = retSt ? RetType::Struct : currentRet();
            lex.advance();                       // past the return type
            if (lex.kind != Tok::Ident) { fail("expected a function name after its return type"); return false; }
            if (fnCount >= kMaxEntryPoints) { fail("too many functions in one class"); return false; }
            // Entry names copy into a fixed buffer, so two sharing a prefix would collide.
            if (lex.identLen > kMaxEntryName) { fail("function name too long"); return false; }
            // The name is captured before the signature, since parsing moves the lexer.
            const char* const fnName = lex.identBeg;
            const uint8_t fnNameLen = static_cast<uint8_t>(lex.identLen);
            // A builtin is found before a script function, so one of the same name would never be called.
            if (isReservedWord(fnName, fnNameLen)) { fail("that name is a reserved word"); return false; }
            if (table.findStruct(fnName, fnNameLen)) { fail("that name is a type"); return false; }
            if (table.find(fnName, fnNameLen)) { fail("that name shadows a built-in function"); return false; }
            lex.advance();                       // the function name
            uint8_t params = 0;
            FnMark& mark = fns[fnCount];
            mark = FnMark{};
            if (!parseParams(&params, &mark.argCount, mark.argStruct)) return false;
            mark.name = fnName;
            mark.nameLen = fnNameLen;
            mark.irStart = static_cast<uint16_t>(ir.count);
            mark.ret = ret;
            mark.params = params;
            mark.retStruct = structIndex(retSt);
            curRet = ret;                        // every return below is checked against it
            curRetStruct = retSt;
            // The IR carries the start INDEX; the lowering turns it into a byte offset.
            ir.fnIrStart[fnCount] = static_cast<uint16_t>(ir.count);
            ir.fnCount = static_cast<uint8_t>(fnCount + 1);
            fnCount++;
            // Parked per function, since a spill before the first prologue has no frame.
            for (VReg v = 0; v < kFirstTemp; v++)
                emit({IrOp::Spill, 0, v, 0,0,0, hostArgSlot(v), nullptr, {}});
            // Copied out before the body can call, which lets one arena block serve every depth.
            for (uint8_t a = 0; a < params; a++) {
                const VReg v = alloc();
                emit({IrOp::LoadCtrl32, v, 0,0,0,0, scriptArgOffset(a), nullptr, {}});
                // Narrowed here, since the callee knows the types and serves every caller once.
                narrowToType(v, paramWordType[a]);
                emit({IrOp::Spill, 0, v, 0,0,0, a, nullptr, {}});
                freeTemp(v);
            }
            if (!parseFunctionBody()) return false;
            any = true;
        }
        if (failed) return false;
        if (!any) { fail("a class with no function does nothing"); return false; }
        return expect(Tok::RBrace, "expected '}' to close the class");
    }
};

}  // namespace

/// Count a source's tokens, stopping once it passes what the IR could hold.
uint32_t countTokens(const char* source) {
    if (!source) return 0;
    uint32_t tokens = 0;
    for (Lexer scan(source); scan.kind != Tok::End && scan.kind != Tok::Error; scan.advance()) {
        if (++tokens > kMaxIrOps) break;   // a runaway source; reserve() rejects past the bound
    }
    return tokens;
}

/// Compile a script to bytes, answering what it declared and where it failed.
CompileResult compileSource(const char* source, const BuiltinTable& table,
                            const SysVarTable& sysvars, uint8_t* out, size_t cap,
                            const RegBudget* squeeze, LowerFn lower,
                            char* strings, uint16_t stringCap) {
    CompileResult r;
    if (!source) { r.error = "no source"; return r; }
    if (!out || cap == 0) { r.error = "no code buffer"; return r; }

    // Sized from the token count against the shipped scripts' ratio; a denser script grows the array as it goes.
    const uint32_t tokens = countTokens(source);
    IrProgram ir;
    // The margin covers a program's fixed overhead, so one token cannot round down to nothing.
    if (!ir.reserve(static_cast<uint16_t>(tokens * kIrOpsPerToken + 8 > kMaxIrOps
                                          ? kMaxIrOps : tokens * kIrOpsPerToken + 8))) {
        r.error = "script too large";
        return r;
    }
    Lexer lex(source);
    Parser parser{lex, table, sysvars, ir, r.className};
    parser.strings = strings;      // where string literals are interned
    parser.stringCap = stringCap;
    if (!parser.parseProgram()) { r.error = parser.error; r.errorCol = parser.errorCol; return r; }
    // The allocator numbers further slots from here up, so the two never overlap.
    ir.localSlots = parser.slotsUsed;

    // A test passes a different ISA's lowerer, so the seam is one function pointer.
    size_t len = lower ? lower(ir, out, cap, squeeze) : lowerToBytes(ir, out, cap, squeeze);
    // A zero length can mean an upstream allocation failed, so report the cause a user can act on.
    if (len == 0) {
        switch (lowerRefusal()) {
            // The numbers live in spillDetail(), so this error stays a literal the result can carry.
            case LowerRefusal::Spill:       r.error = kSpillRefused; break;
            case LowerRefusal::NullCall:    r.error = "codegen failed: a builtin has no function on this target"; break;
            case LowerRefusal::AsmOverflow: r.error = "codegen failed: assembler overflow (branch range, slot, or immediate)"; break;
            case LowerRefusal::OverCap:     r.error = "codegen failed: code larger than its buffer"; break;
            case LowerRefusal::NoMemory:    r.error = "codegen failed: out of memory for the assembler"; break;
            default:                        r.error = kCodegenFailed; break;
        }
        return r;
    }
    r.ok = true;
    r.len = len;
    // Surfaced so the binding can create real MoonModule controls.
    r.stringLen = parser.stringLen;
    r.memberCount = parser.memberCount;
    for (uint8_t i = 0; i < parser.memberCount; i++) r.members[i] = parser.members[i];
    // A real symbol table: the parser recorded an IR index and the lowering converted it.
    r.entryCount = parser.fnCount;
    for (uint8_t i = 0; i < parser.fnCount; i++)
        // The declared return type travels to the engine, or runValue refuses to answer.
        r.entries[i] = {parser.fns[i].name, parser.fns[i].nameLen, ir.fnOffset[i],
                        parser.fns[i].ret};
    return r;
}

}  // namespace mm::moonlive
