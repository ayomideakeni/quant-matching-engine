// Pre-trade risk engine (Challenge B).
//
// Single-account risk gate speaking JSON Lines on stdin/stdout. Every
// committed request (accepted or rejected) is appended to an append-only
// journal in the state directory *before* its response is written to stdout.
// Recovery replays the journal through the same deterministic state machine.
//
// See ../README.md for the design, durability argument and cost measurements.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "journal encoding assumes a little-endian host"
#endif

namespace {

constexpr size_t kMaxLineContent = 65535;          // 64 KiB including '\n'
constexpr uint32_t kMaxRecord = 64u << 20;         // sanity bound on a journal record
constexpr size_t kFlushThreshold = 4u << 20;       // bound on buffered journal/stdout bytes
constexpr int64_t kMaxLimit = 1000000000000000000LL;

// ------------------------------------------------------------------ utilities

[[noreturn]] void die_io(const char* what, int err) {
    std::fprintf(stderr, "risk_engine: I/O failure: %s: %s\n", what, std::strerror(err));
    std::_Exit(3);
}

[[noreturn]] void die_corrupt(const char* what) {
    std::fprintf(stderr, "risk_engine: journal is inconsistent: %s\n", what);
    std::_Exit(3);
}

bool write_all(int fd, const char* p, size_t n, int& err) {
    while (n > 0) {
        ssize_t k = ::write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            err = errno;
            return false;
        }
        if (k == 0) {
            err = EIO;
            return false;
        }
        p += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

// CRC-32C (Castagnoli), slicing-by-8.
uint32_t g_crc[8][256];

void crc_init() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        g_crc[0][i] = c;
    }
    for (int t = 1; t < 8; ++t)
        for (uint32_t i = 0; i < 256; ++i)
            g_crc[t][i] = (g_crc[t - 1][i] >> 8) ^ g_crc[0][g_crc[t - 1][i] & 0xFF];
}

uint32_t crc32c(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    while (n >= 8) {
        uint32_t a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= c;
        c = g_crc[7][a & 0xFF] ^ g_crc[6][(a >> 8) & 0xFF] ^ g_crc[5][(a >> 16) & 0xFF] ^
            g_crc[4][a >> 24] ^ g_crc[3][b & 0xFF] ^ g_crc[2][(b >> 8) & 0xFF] ^
            g_crc[1][(b >> 16) & 0xFF] ^ g_crc[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) c = (c >> 8) ^ g_crc[0][(c ^ *p++) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}

void put_u8(std::string& s, uint8_t v) { s.push_back(static_cast<char>(v)); }
void put_u32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
void put_i64(std::string& s, int64_t v) { s.append(reinterpret_cast<const char*>(&v), 8); }
void put_str8(std::string& s, const std::string& v) {
    put_u8(s, static_cast<uint8_t>(v.size()));
    s.append(v);
}

uint32_t get_u32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t get_u64(const char* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

uint64_t hash_bytes(std::string_view s) {
    // FNV-1a 64 with a final avalanche; identifiers are short ASCII strings.
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

void append_int(std::string& s, int64_t v) {
    char buf[24];
    int n = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    s.append(buf, static_cast<size_t>(n));
}

// ------------------------------------------------------------------ strict JSON

struct JVal {
    enum Kind : uint8_t { OBJ, ARR, STR, INT } kind = INT;
    int64_t i = 0;
    std::string s;
    std::vector<std::string> keys;  // OBJ
    std::vector<JVal> vals;         // OBJ values / ARR elements

    const JVal* member(std::string_view k) const {
        for (size_t n = 0; n < keys.size(); ++n)
            if (keys[n] == k) return &vals[n];
        return nullptr;
    }
};

// Accepts exactly RFC 8259 JSON, except that everything this protocol can
// never accept (true/false/null, fractions, exponents, negative non-zero
// integers, integers beyond int64, non-ASCII text) is rejected while parsing.
// Every such line would be a fatal schema error anyway.
struct Parser {
    const char* p;
    const char* end;
    const char* err = nullptr;
    int depth = 0;

    bool fail(const char* m) {
        if (!err) err = m;
        return false;
    }
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool document(JVal& v) {
        if (!value(v)) return false;
        ws();
        if (p != end) return fail("trailing characters after JSON value");
        return true;
    }

    bool value(JVal& v) {
        ws();
        if (p >= end) return fail("unexpected end of line");
        char c = *p;
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') {
            v.kind = JVal::STR;
            return string(v.s);
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        if (c == 't' || c == 'f' || c == 'n') return fail("boolean/null value is not allowed");
        return fail("invalid JSON value");
    }

    bool object(JVal& v) {
        if (++depth > 16) return fail("nesting too deep");
        v.kind = JVal::OBJ;
        ++p;
        ws();
        if (p < end && *p == '}') {
            ++p;
            --depth;
            return true;
        }
        for (;;) {
            ws();
            if (p >= end || *p != '"') return fail("expected object key");
            std::string key;
            if (!string(key)) return false;
            for (const auto& k : v.keys)
                if (k == key) return fail("duplicate object key");
            ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            ++p;
            v.keys.push_back(std::move(key));
            v.vals.emplace_back();
            if (!value(v.vals.back())) return false;
            ws();
            if (p >= end) return fail("unterminated object");
            if (*p == ',') {
                ++p;
                continue;
            }
            if (*p == '}') {
                ++p;
                --depth;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(JVal& v) {
        if (++depth > 16) return fail("nesting too deep");
        v.kind = JVal::ARR;
        ++p;
        ws();
        if (p < end && *p == ']') {
            ++p;
            --depth;
            return true;
        }
        for (;;) {
            v.vals.emplace_back();
            if (!value(v.vals.back())) return false;
            ws();
            if (p >= end) return fail("unterminated array");
            if (*p == ',') {
                ++p;
                continue;
            }
            if (*p == ']') {
                ++p;
                --depth;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    static int hexval(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool string(std::string& out) {
        ++p;  // opening quote
        for (;;) {
            if (p >= end) return fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"') {
                ++p;
                return true;
            }
            if (c < 0x20) return fail("control character in string");
            if (c >= 0x80) return fail("non-ASCII text cannot match any field");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++p;
                continue;
            }
            ++p;
            if (p >= end) return fail("unterminated escape");
            char e = *p++;
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (end - p < 4) return fail("short \\u escape");
                    int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        int h = hexval(p[k]);
                        if (h < 0) return fail("bad \\u escape");
                        cp = cp * 16 + h;
                    }
                    p += 4;
                    if (cp >= 0x80) return fail("non-ASCII text cannot match any field");
                    out.push_back(static_cast<char>(cp));
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool number(JVal& v) {
        bool neg = false;
        if (*p == '-') {
            neg = true;
            ++p;
        }
        if (p >= end || *p < '0' || *p > '9') return fail("invalid number");
        uint64_t acc = 0;
        if (*p == '0') {
            ++p;
        } else {
            while (p < end && *p >= '0' && *p <= '9') {
                uint64_t d = static_cast<uint64_t>(*p - '0');
                if (acc > (static_cast<uint64_t>(INT64_MAX) - d) / 10) return fail("integer out of range");
                acc = acc * 10 + d;
                ++p;
            }
        }
        if (p < end && (*p == '.' || *p == 'e' || *p == 'E')) return fail("non-integer number");
        if (p < end && *p >= '0' && *p <= '9') return fail("leading zero in number");
        if (neg && acc != 0) return fail("negative integer out of range");
        v.kind = JVal::INT;
        v.i = static_cast<int64_t>(acc);
        return true;
    }
};

// ------------------------------------------------------------------ requests

enum Type : uint8_t { T_INIT = 1, T_NEW, T_FILL, T_CANCEL, T_LIMITS, T_QUERY };
enum Side : uint8_t { BUY = 0, SELL = 1 };

struct Limits {
    int64_t max_order_qty = 0, max_order_notional = 0, max_abs_position = 0, max_gross_exposure = 0;
};

struct SymDef {
    std::string symbol;
    int64_t reference_price;
};

struct Req {
    Type type = T_QUERY;
    std::string event_id;
    std::string order_id;
    std::string symbol;
    Side side = BUY;
    int64_t qty = 0;
    int64_t price = 0;
    Limits limits;
    std::vector<SymDef> symbols;  // sorted by symbol
};

bool is_id(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool is_symbol(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    if (s[0] < 'A' || s[0] > 'Z') return false;
    for (char c : s) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool exact_keys(const JVal& o, std::initializer_list<const char*> want) {
    if (o.kind != JVal::OBJ || o.keys.size() != want.size()) return false;
    for (const char* k : want)
        if (!o.member(k)) return false;
    return true;  // keys are unique, so equal count + all present == exact set
}

bool get_int(const JVal* v, int64_t lo, int64_t hi, int64_t& out) {
    if (!v || v->kind != JVal::INT || v->i < lo || v->i > hi) return false;
    out = v->i;
    return true;
}

bool get_limits(const JVal* v, Limits& L) {
    if (!v || !exact_keys(*v, {"max_order_qty", "max_order_notional", "max_abs_position", "max_gross_exposure"}))
        return false;
    return get_int(v->member("max_order_qty"), 0, kMaxLimit, L.max_order_qty) &&
           get_int(v->member("max_order_notional"), 0, kMaxLimit, L.max_order_notional) &&
           get_int(v->member("max_abs_position"), 0, kMaxLimit, L.max_abs_position) &&
           get_int(v->member("max_gross_exposure"), 0, kMaxLimit, L.max_gross_exposure);
}

bool get_string(const JVal* v, std::string& out) {
    if (!v || v->kind != JVal::STR) return false;
    out = v->s;
    return true;
}

// Schema validation. Returns nullptr on success, else a diagnostic.
const char* validate(const JVal& root, Req& r) {
    if (root.kind != JVal::OBJ) return "line is not a JSON object";
    const JVal* t = root.member("type");
    if (!t || t->kind != JVal::STR) return "missing or non-string 'type'";
    const std::string& ts = t->s;
    bool keys_ok;
    if (ts == "init") {
        r.type = T_INIT;
        keys_ok = exact_keys(root, {"type", "event_id", "symbols", "limits"});
    } else if (ts == "new") {
        r.type = T_NEW;
        keys_ok = exact_keys(root, {"type", "event_id", "order_id", "symbol", "side", "qty", "price"});
    } else if (ts == "fill") {
        r.type = T_FILL;
        keys_ok = exact_keys(root, {"type", "event_id", "order_id", "qty"});
    } else if (ts == "cancel") {
        r.type = T_CANCEL;
        keys_ok = exact_keys(root, {"type", "event_id", "order_id"});
    } else if (ts == "limits") {
        r.type = T_LIMITS;
        keys_ok = exact_keys(root, {"type", "event_id", "limits"});
    } else if (ts == "query") {
        r.type = T_QUERY;
        keys_ok = exact_keys(root, {"type", "event_id"});
    } else {
        return "unknown request type";
    }
    if (!keys_ok) return "wrong set of fields for request type";
    if (!get_string(root.member("event_id"), r.event_id) || !is_id(r.event_id)) return "invalid event_id";

    switch (r.type) {
        case T_INIT: {
            const JVal* syms = root.member("symbols");
            if (syms->kind != JVal::ARR || syms->vals.empty() || syms->vals.size() > 32)
                return "symbols must be an array of 1-32 objects";
            for (const JVal& e : syms->vals) {
                if (!exact_keys(e, {"symbol", "reference_price"})) return "invalid symbol definition";
                SymDef d;
                if (!get_string(e.member("symbol"), d.symbol) || !is_symbol(d.symbol)) return "invalid symbol";
                if (!get_int(e.member("reference_price"), 1, 1000000, d.reference_price))
                    return "invalid reference_price";
                r.symbols.push_back(std::move(d));
            }
            std::sort(r.symbols.begin(), r.symbols.end(),
                      [](const SymDef& a, const SymDef& b) { return a.symbol < b.symbol; });
            for (size_t i = 1; i < r.symbols.size(); ++i)
                if (r.symbols[i].symbol == r.symbols[i - 1].symbol) return "duplicate symbol in init";
            if (!get_limits(root.member("limits"), r.limits)) return "invalid limits";
            break;
        }
        case T_NEW: {
            if (!get_string(root.member("order_id"), r.order_id) || !is_id(r.order_id)) return "invalid order_id";
            if (!get_string(root.member("symbol"), r.symbol) || !is_symbol(r.symbol)) return "invalid symbol";
            std::string side;
            if (!get_string(root.member("side"), side)) return "invalid side";
            if (side == "buy")
                r.side = BUY;
            else if (side == "sell")
                r.side = SELL;
            else
                return "invalid side";
            if (!get_int(root.member("qty"), 1, 1000000, r.qty)) return "invalid qty";
            if (!get_int(root.member("price"), 1, 1000000, r.price)) return "invalid price";
            break;
        }
        case T_FILL:
            if (!get_string(root.member("order_id"), r.order_id) || !is_id(r.order_id)) return "invalid order_id";
            if (!get_int(root.member("qty"), 1, 1000000, r.qty)) return "invalid qty";
            break;
        case T_CANCEL:
            if (!get_string(root.member("order_id"), r.order_id) || !is_id(r.order_id)) return "invalid order_id";
            break;
        case T_LIMITS:
            if (!get_limits(root.member("limits"), r.limits)) return "invalid limits";
            break;
        case T_QUERY:
            break;
    }
    return nullptr;
}

// Canonical binary encoding of everything except event_id. Two requests are
// "semantically identical" iff their encodings are byte-equal: key order and
// whitespace are gone, string escapes are decoded and init.symbols is sorted.
void encode_canon(const Req& r, std::string& out) {
    out.clear();
    put_u8(out, r.type);
    switch (r.type) {
        case T_INIT:
            put_u8(out, static_cast<uint8_t>(r.symbols.size()));
            for (const SymDef& d : r.symbols) {
                put_str8(out, d.symbol);
                put_u32(out, static_cast<uint32_t>(d.reference_price));
            }
            [[fallthrough]];
        case T_LIMITS:
            put_i64(out, r.limits.max_order_qty);
            put_i64(out, r.limits.max_order_notional);
            put_i64(out, r.limits.max_abs_position);
            put_i64(out, r.limits.max_gross_exposure);
            break;
        case T_NEW:
            put_str8(out, r.order_id);
            put_str8(out, r.symbol);
            put_u8(out, r.side);
            put_u32(out, static_cast<uint32_t>(r.qty));
            put_u32(out, static_cast<uint32_t>(r.price));
            break;
        case T_FILL:
            put_str8(out, r.order_id);
            put_u32(out, static_cast<uint32_t>(r.qty));
            break;
        case T_CANCEL:
            put_str8(out, r.order_id);
            break;
        case T_QUERY:
            break;
    }
}

// Inverse of encode_canon, used only during recovery.
bool decode_canon(const char* p, size_t n, Req& r) {
    const char* e = p + n;
    auto u8 = [&](uint8_t& v) {
        if (e - p < 1) return false;
        v = static_cast<uint8_t>(*p++);
        return true;
    };
    auto u32 = [&](int64_t& v) {
        if (e - p < 4) return false;
        v = get_u32(p);
        p += 4;
        return true;
    };
    auto i64 = [&](int64_t& v) {
        if (e - p < 8) return false;
        v = static_cast<int64_t>(get_u64(p));
        p += 8;
        return true;
    };
    auto str = [&](std::string& v) {
        uint8_t len;
        if (!u8(len) || e - p < len) return false;
        v.assign(p, len);
        p += len;
        return true;
    };
    auto lim = [&](Limits& L) {
        return i64(L.max_order_qty) && i64(L.max_order_notional) && i64(L.max_abs_position) &&
               i64(L.max_gross_exposure);
    };
    uint8_t t;
    if (!u8(t) || t < T_INIT || t > T_QUERY) return false;
    r.type = static_cast<Type>(t);
    switch (r.type) {
        case T_INIT: {
            uint8_t cnt;
            if (!u8(cnt)) return false;
            r.symbols.resize(cnt);
            for (SymDef& d : r.symbols)
                if (!str(d.symbol) || !u32(d.reference_price)) return false;
            if (!lim(r.limits)) return false;
            break;
        }
        case T_LIMITS:
            if (!lim(r.limits)) return false;
            break;
        case T_NEW: {
            uint8_t side;
            if (!str(r.order_id) || !str(r.symbol) || !u8(side) || side > SELL || !u32(r.qty) || !u32(r.price))
                return false;
            r.side = static_cast<Side>(side);
            break;
        }
        case T_FILL:
            if (!str(r.order_id) || !u32(r.qty)) return false;
            break;
        case T_CANCEL:
            if (!str(r.order_id)) return false;
            break;
        case T_QUERY:
            break;
    }
    return p == e;
}

// ------------------------------------------------------------------ business state

enum Code : uint8_t {
    ACCEPTED = 0,
    NOT_INITIALISED,
    ALREADY_INITIALISED,
    DUPLICATE_ORDER_ID,
    UNKNOWN_SYMBOL,
    ORDER_QTY,
    ORDER_NOTIONAL,
    POSITION_LIMIT,
    GROSS_EXPOSURE,
    UNKNOWN_ORDER,
    ORDER_CLOSED,
    INSUFFICIENT_REMAINING,
    NUM_CODES
};

const char* const kReason[NUM_CODES] = {
    "",
    "not_initialised",
    "already_initialised",
    "duplicate_order_id",
    "unknown_symbol",
    "order_qty",
    "order_notional",
    "position_limit",
    "gross_exposure",
    "unknown_order",
    "order_closed",
    "insufficient_remaining",
};

struct Symbol {
    std::string name;
    int64_t reference_price;
    int64_t position = 0;  // P
    int64_t buy_rem = 0;   // B
    int64_t sell_rem = 0;  // S
};

struct Order {
    uint8_t sym;
    Side side;
    bool open;
    int64_t remaining;
    int64_t price;
};

int64_t iabs(int64_t v) { return v < 0 ? -v : v; }

class Engine {
   public:
    Engine() { order_index_.reserve(1 << 18); }

    // Applies a schema-valid, non-retried request; mutates only on acceptance.
    uint8_t apply(const Req& r) {
        if (r.type == T_INIT) {
            if (initialised_) return ALREADY_INITIALISED;
            initialised_ = true;
            limits_ = r.limits;
            for (const SymDef& d : r.symbols) symbols_.push_back(Symbol{d.symbol, d.reference_price});
            return ACCEPTED;
        }
        if (!initialised_) return NOT_INITIALISED;
        switch (r.type) {
            case T_NEW: return apply_new(r);
            case T_FILL: return apply_fill(r);
            case T_CANCEL: return apply_cancel(r);
            case T_LIMITS: limits_ = r.limits; return ACCEPTED;
            case T_QUERY: return ACCEPTED;
            default: return ACCEPTED;  // unreachable
        }
    }

    void query_json(const std::string& event_id, std::string& o) const {
        o.clear();
        o += "{\"event_id\":\"";
        o += event_id;
        o += "\",\"status\":\"accepted\",\"state\":{\"limits\":{\"max_order_qty\":";
        append_int(o, limits_.max_order_qty);
        o += ",\"max_order_notional\":";
        append_int(o, limits_.max_order_notional);
        o += ",\"max_abs_position\":";
        append_int(o, limits_.max_abs_position);
        o += ",\"max_gross_exposure\":";
        append_int(o, limits_.max_gross_exposure);
        o += "},\"gross_exposure\":";
        append_int(o, filled_exposure_ + reserved_exposure_);
        o += ",\"positions\":[";
        for (size_t i = 0; i < symbols_.size(); ++i) {
            const Symbol& s = symbols_[i];
            if (i) o += ',';
            o += "{\"symbol\":\"";
            o += s.name;
            o += "\",\"position\":";
            append_int(o, s.position);
            o += ",\"buy_remaining\":";
            append_int(o, s.buy_rem);
            o += ",\"sell_remaining\":";
            append_int(o, s.sell_rem);
            o += '}';
        }
        o += "],\"orders\":[";
        bool first = true;
        for (const auto& [id, idx] : open_orders_) {
            const Order& ord = orders_[idx];
            if (!first) o += ',';
            first = false;
            o += "{\"order_id\":\"";
            o += id;
            o += "\",\"symbol\":\"";
            o += symbols_[ord.sym].name;
            o += ord.side == BUY ? "\",\"side\":\"buy\",\"remaining\":" : "\",\"side\":\"sell\",\"remaining\":";
            append_int(o, ord.remaining);
            o += ",\"price\":";
            append_int(o, ord.price);
            o += '}';
        }
        o += "]}}\n";
    }

   private:
    int find_symbol(const std::string& name) const {
        auto it = std::lower_bound(symbols_.begin(), symbols_.end(), name,
                                   [](const Symbol& s, const std::string& n) { return s.name < n; });
        if (it == symbols_.end() || it->name != name) return -1;
        return static_cast<int>(it - symbols_.begin());
    }

    void set_position(Symbol& s, int64_t p) {
        filled_exposure_ -= iabs(s.position) * s.reference_price;
        s.position = p;
        filled_exposure_ += iabs(s.position) * s.reference_price;
    }

    uint8_t apply_new(const Req& r) {
        if (order_index_.count(r.order_id)) return DUPLICATE_ORDER_ID;
        int si = find_symbol(r.symbol);
        if (si < 0) return UNKNOWN_SYMBOL;
        if (r.qty > limits_.max_order_qty) return ORDER_QTY;
        int64_t notional = r.qty * r.price;
        if (notional > limits_.max_order_notional) return ORDER_NOTIONAL;
        Symbol& s = symbols_[si];
        int64_t B = s.buy_rem + (r.side == BUY ? r.qty : 0);
        int64_t S = s.sell_rem + (r.side == SELL ? r.qty : 0);
        if (iabs(s.position + B) > limits_.max_abs_position || iabs(s.position - S) > limits_.max_abs_position)
            return POSITION_LIMIT;
        if (filled_exposure_ + reserved_exposure_ + notional > limits_.max_gross_exposure) return GROSS_EXPOSURE;

        s.buy_rem = B;
        s.sell_rem = S;
        reserved_exposure_ += notional;
        uint32_t idx = static_cast<uint32_t>(orders_.size());
        orders_.push_back(Order{static_cast<uint8_t>(si), r.side, true, r.qty, r.price});
        order_index_.emplace(r.order_id, idx);
        open_orders_.emplace(r.order_id, idx);
        return ACCEPTED;
    }

    uint8_t apply_fill(const Req& r) {
        auto it = order_index_.find(r.order_id);
        if (it == order_index_.end()) return UNKNOWN_ORDER;
        Order& o = orders_[it->second];
        if (!o.open) return ORDER_CLOSED;
        if (r.qty > o.remaining) return INSUFFICIENT_REMAINING;
        Symbol& s = symbols_[o.sym];
        o.remaining -= r.qty;
        reserved_exposure_ -= r.qty * o.price;
        if (o.side == BUY) {
            s.buy_rem -= r.qty;
            set_position(s, s.position + r.qty);
        } else {
            s.sell_rem -= r.qty;
            set_position(s, s.position - r.qty);
        }
        if (o.remaining == 0) {
            o.open = false;
            open_orders_.erase(r.order_id);
        }
        return ACCEPTED;
    }

    uint8_t apply_cancel(const Req& r) {
        auto it = order_index_.find(r.order_id);
        if (it == order_index_.end()) return UNKNOWN_ORDER;
        Order& o = orders_[it->second];
        if (!o.open) return ORDER_CLOSED;
        Symbol& s = symbols_[o.sym];
        reserved_exposure_ -= o.remaining * o.price;
        (o.side == BUY ? s.buy_rem : s.sell_rem) -= o.remaining;
        o.remaining = 0;
        o.open = false;
        open_orders_.erase(r.order_id);
        return ACCEPTED;
    }

    bool initialised_ = false;
    Limits limits_;
    std::vector<Symbol> symbols_;  // sorted by name (init input is sorted)
    std::vector<Order> orders_;    // every accepted order, open or closed
    std::unordered_map<std::string, uint32_t> order_index_;
    std::map<std::string, uint32_t> open_orders_;  // ASCII order for queries
    int64_t filled_exposure_ = 0;    // sum |P| * reference_price
    int64_t reserved_exposure_ = 0;  // sum remaining * order price over open orders
};

// ------------------------------------------------------------------ idempotency records

// Every committed request is remembered here: event_id, canonical payload,
// result code and (for accepted queries) where the historical response lives
// in the journal. Records live in a chunked arena (no per-record allocation,
// no reallocation copies) and are indexed by an open-addressing table.
//
// Record layout: u8 id_len | id | u32 canon_len | canon | u8 code | u64 resp_off | u32 resp_len
class EventStore {
   public:
    struct View {
        std::string_view canon;
        uint8_t code;
        uint64_t resp_off;
        uint32_t resp_len;
    };

    EventStore() { table_.assign(1 << 16, Slot{}); }

    bool find(std::string_view id, View& out) const {
        uint64_t h = hash_bytes(id);
        size_t mask = table_.size() - 1;
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            const Slot& s = table_[i];
            if (!s.rec) return false;
            if (s.hash == h && id_of(s.rec) == id) {
                out = view_of(s.rec);
                return true;
            }
        }
    }

    void insert(std::string_view id, std::string_view canon, uint8_t code, uint64_t resp_off, uint32_t resp_len) {
        size_t need = 1 + id.size() + 4 + canon.size() + 1 + 8 + 4;
        char* rec = alloc(need);
        char* p = rec;
        *p++ = static_cast<char>(id.size());
        std::memcpy(p, id.data(), id.size());
        p += id.size();
        uint32_t cl = static_cast<uint32_t>(canon.size());
        std::memcpy(p, &cl, 4);
        p += 4;
        std::memcpy(p, canon.data(), canon.size());
        p += canon.size();
        *p++ = static_cast<char>(code);
        std::memcpy(p, &resp_off, 8);
        p += 8;
        std::memcpy(p, &resp_len, 4);

        if ((count_ + 1) * 2 > table_.size()) grow();
        place(Slot{rec, hash_bytes(id)});
        ++count_;
    }

   private:
    struct Slot {
        const char* rec = nullptr;
        uint64_t hash = 0;
    };

    static std::string_view id_of(const char* rec) {
        return std::string_view(rec + 1, static_cast<unsigned char>(rec[0]));
    }
    static View view_of(const char* rec) {
        const char* p = rec + 1 + static_cast<unsigned char>(rec[0]);
        uint32_t cl = get_u32(p);
        p += 4;
        View v;
        v.canon = std::string_view(p, cl);
        p += cl;
        v.code = static_cast<uint8_t>(*p++);
        v.resp_off = get_u64(p);
        v.resp_len = get_u32(p + 8);
        return v;
    }

    void place(Slot s) {
        size_t mask = table_.size() - 1;
        size_t i = s.hash & mask;
        while (table_[i].rec) i = (i + 1) & mask;
        table_[i] = s;
    }

    void grow() {
        std::vector<Slot> old;
        old.swap(table_);
        table_.assign(old.size() * 2, Slot{});
        for (const Slot& s : old)
            if (s.rec) place(s);
    }

    char* alloc(size_t n) {
        if (n > left_) {
            size_t sz = std::max<size_t>(n, 1u << 20);
            blocks_.emplace_back(new char[sz]);
            cur_ = blocks_.back().get();
            left_ = sz;
        }
        char* r = cur_;
        cur_ += n;
        left_ -= n;
        return r;
    }

    std::vector<std::unique_ptr<char[]>> blocks_;
    char* cur_ = nullptr;
    size_t left_ = 0;
    std::vector<Slot> table_;
    size_t count_ = 0;
};

// ------------------------------------------------------------------ journal

// Append-only file of framed records: u32 payload_len | u32 crc32c(payload) | payload
// payload: u8 id_len | id | u32 canon_len | canon | u8 code | response bytes (accepted queries only)
//
// A record is committed once write(2) has returned for all of its bytes: the
// data is then in the kernel page cache and survives the death of this
// process. SIGKILL can interrupt a write and leave a *prefix* of the batch on
// disk; recovery discards an incomplete tail record.
class Journal {
   public:
    int fd = -1;
    uint64_t flushed_end = 0;  // file offset of the end of written data
    std::string pending;       // encoded records not yet written

    uint64_t logical_end() const { return flushed_end + pending.size(); }

    // Appends a record; returns the absolute file offset of the response bytes.
    uint64_t append(std::string_view id, std::string_view canon, uint8_t code, std::string_view resp) {
        size_t start = pending.size();
        uint32_t len = static_cast<uint32_t>(1 + id.size() + 4 + canon.size() + 1 + resp.size());
        pending.append(8, '\0');
        put_u8(pending, static_cast<uint8_t>(id.size()));
        pending.append(id);
        put_u32(pending, static_cast<uint32_t>(canon.size()));
        pending.append(canon);
        put_u8(pending, code);
        uint64_t resp_off = flushed_end + pending.size();
        pending.append(resp);
        uint32_t crc = crc32c(pending.data() + start + 8, len);
        std::memcpy(&pending[start], &len, 4);
        std::memcpy(&pending[start + 4], &crc, 4);
        return resp_off;
    }

    void flush() {
        if (pending.empty()) return;
        int err = 0;
        if (!write_all(fd, pending.data(), pending.size(), err)) die_io("journal write", err);
        flushed_end += pending.size();
        pending.clear();
    }

    void read_at(uint64_t off, uint32_t len, std::string& out) const {
        out.resize(len);
        if (off >= flushed_end) {
            std::memcpy(&out[0], pending.data() + (off - flushed_end), len);
            return;
        }
        size_t got = 0;
        while (got < len) {
            ssize_t k = ::pread(fd, &out[got], len - got, static_cast<off_t>(off + got));
            if (k < 0) {
                if (errno == EINTR) continue;
                die_io("journal read", errno);
            }
            if (k == 0) die_corrupt("historical response is missing");
            got += static_cast<size_t>(k);
        }
    }
};

// ------------------------------------------------------------------ the process

class Server {
   public:
    explicit Server(const std::string& state_dir) {
        std::string path = state_dir + "/journal.bin";
        journal_.fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (journal_.fd < 0) die_io("open journal", errno);
        recover();
    }

    int run() {
        const size_t cap = 4u << 20;
        std::unique_ptr<char[]> buf(new char[cap]);
        size_t b = 0, e = 0;
        bool eof = false;
        for (;;) {
            for (;;) {
                char* nl = static_cast<char*>(std::memchr(buf.get() + b, '\n', e - b));
                if (!nl) break;
                size_t n = static_cast<size_t>(nl - (buf.get() + b));
                ++line_no_;
                if (n > kMaxLineContent) fatal("line exceeds 64 KiB");
                handle_line(buf.get() + b, n);
                b += n + 1;
                if (journal_.pending.size() > kFlushThreshold || out_.size() > kFlushThreshold) flush();
            }
            if (eof) {
                if (e > b) {  // final line without a trailing newline
                    ++line_no_;
                    if (e - b > kMaxLineContent) fatal("line exceeds 64 KiB");
                    handle_line(buf.get() + b, e - b);
                }
                flush();
                return 0;
            }
            if (e - b > kMaxLineContent) {
                ++line_no_;
                fatal("line exceeds 64 KiB");
            }
            std::memmove(buf.get(), buf.get() + b, e - b);
            e -= b;
            b = 0;
            // Everything decided so far becomes durable and is acknowledged
            // before we might block waiting for more input.
            flush();
            ssize_t k;
            do {
                k = ::read(0, buf.get() + e, cap - e);
            } while (k < 0 && errno == EINTR);
            if (k < 0) die_io("read stdin", errno);
            if (k == 0)
                eof = true;
            else
                e += static_cast<size_t>(k);
        }
    }

   private:
    // Journal bytes first, then responses: a response is never visible
    // before its record is in the page cache.
    void flush() {
        journal_.flush();
        if (!out_.empty()) {
            int err = 0;
            if (!write_all(1, out_.data(), out_.size(), err)) die_io("write stdout", err);
            out_.clear();
        }
    }

    [[noreturn]] void fatal(const char* msg) {
        flush();  // preceding valid responses
        std::fprintf(stderr, "risk_engine: protocol error on input line %llu: %s\n",
                     static_cast<unsigned long long>(line_no_), msg);
        std::_Exit(2);
    }

    void emit_simple(const std::string& id, uint8_t code) {
        out_ += "{\"event_id\":\"";
        out_ += id;
        if (code == ACCEPTED) {
            out_ += "\",\"status\":\"accepted\"}\n";
        } else {
            out_ += "\",\"status\":\"rejected\",\"reason\":\"";
            out_ += kReason[code];
            out_ += "\"}\n";
        }
    }

    void handle_line(const char* p, size_t n) {
        JVal root;
        Parser ps{p, p + n};
        if (!ps.document(root)) fatal(ps.err);
        Req r;
        if (const char* err = validate(root, r)) fatal(err);
        encode_canon(r, canon_);

        EventStore::View prior;
        if (events_.find(r.event_id, prior)) {
            if (prior.canon != std::string_view(canon_)) {
                out_ += "{\"event_id\":\"";
                out_ += r.event_id;
                out_ += "\",\"status\":\"rejected\",\"reason\":\"event_id_conflict\"}\n";
            } else if (prior.resp_len > 0) {
                journal_.read_at(prior.resp_off, prior.resp_len, scratch_);
                out_ += scratch_;
            } else {
                emit_simple(r.event_id, prior.code);
            }
            return;
        }

        uint8_t code = engine_.apply(r);
        if (r.type == T_QUERY && code == ACCEPTED) {
            engine_.query_json(r.event_id, scratch_);
            uint64_t off = journal_.append(r.event_id, canon_, code, scratch_);
            events_.insert(r.event_id, canon_, code, off, static_cast<uint32_t>(scratch_.size()));
            out_ += scratch_;
        } else {
            journal_.append(r.event_id, canon_, code, {});
            events_.insert(r.event_id, canon_, code, 0, 0);
            emit_simple(r.event_id, code);
        }
    }

    void recover() {
        struct stat st;
        if (::fstat(journal_.fd, &st) != 0) die_io("stat journal", errno);
        const uint64_t file_size = static_cast<uint64_t>(st.st_size);

        std::vector<char> buf(8u << 20);
        size_t b = 0, e = 0;
        uint64_t pos = 0;  // file offset of buf[b]
        bool eof = false;
        auto ensure = [&](size_t n) {
            while (e - b < n && !eof) {
                if (b > 0) {
                    std::memmove(buf.data(), buf.data() + b, e - b);
                    e -= b;
                    b = 0;
                }
                if (buf.size() < n) buf.resize(std::max(buf.size() * 2, n));
                ssize_t k = ::pread(journal_.fd, buf.data() + e, buf.size() - e, static_cast<off_t>(pos + e));
                if (k < 0) {
                    if (errno == EINTR) continue;
                    die_io("read journal", errno);
                }
                if (k == 0)
                    eof = true;
                else
                    e += static_cast<size_t>(k);
            }
            return e - b >= n;
        };

        Req r;
        std::string canon2;
        for (;;) {
            if (!ensure(8)) break;  // clean end or torn header
            uint32_t len = get_u32(buf.data() + b);
            uint32_t crc = get_u32(buf.data() + b + 4);
            if (len < 7 || len > kMaxRecord) die_corrupt("bad record length");
            if (!ensure(8 + static_cast<size_t>(len))) break;  // torn tail record
            const char* p = buf.data() + b + 8;
            if (crc32c(p, len) != crc) die_corrupt("record checksum mismatch");

            const char* end = p + len;
            size_t idl = static_cast<unsigned char>(*p++);
            if (idl == 0 || static_cast<size_t>(end - p) < idl + 4) die_corrupt("bad record");
            std::string_view id(p, idl);
            p += idl;
            uint32_t cl = get_u32(p);
            p += 4;
            if (static_cast<size_t>(end - p) < static_cast<size_t>(cl) + 1) die_corrupt("bad record");
            std::string_view canon(p, cl);
            p += cl;
            uint8_t code = static_cast<uint8_t>(*p++);
            uint32_t resp_len = static_cast<uint32_t>(end - p);
            uint64_t resp_off = pos + static_cast<uint64_t>(p - (buf.data() + b));

            r = Req{};
            if (!decode_canon(canon.data(), canon.size(), r)) die_corrupt("undecodable request");
            r.event_id.assign(id);
            EventStore::View dummy;
            if (events_.find(id, dummy)) die_corrupt("duplicate event id in journal");
            uint8_t replayed = engine_.apply(r);
            if (replayed != code) die_corrupt("replay diverged from recorded decision");
            bool has_resp = (r.type == T_QUERY && code == ACCEPTED);
            if (has_resp != (resp_len > 0)) die_corrupt("bad query record");
            events_.insert(id, canon, code, has_resp ? resp_off : 0, resp_len);

            b += 8 + len;
            pos += 8 + len;
        }
        if (pos != file_size) {
            // Only an interrupted append can leave bytes past the last
            // complete record; they were never acknowledged.
            if (::ftruncate(journal_.fd, static_cast<off_t>(pos)) != 0) die_io("truncate torn journal tail", errno);
        }
        journal_.flushed_end = pos;
    }

    Engine engine_;
    EventStore events_;
    Journal journal_;
    std::string out_;
    std::string canon_;
    std::string scratch_;
    uint64_t line_no_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string dir;
    if (argc == 3 && std::strcmp(argv[1], "--state-dir") == 0) {
        dir = argv[2];
    } else if (argc == 2 && std::strncmp(argv[1], "--state-dir=", 12) == 0) {
        dir = argv[1] + 12;
    } else {
        std::fprintf(stderr, "usage: %s --state-dir /absolute/path\n", argv[0]);
        return 64;
    }
    if (dir.empty()) {
        std::fprintf(stderr, "risk_engine: empty --state-dir\n");
        return 64;
    }
    ::signal(SIGPIPE, SIG_IGN);  // report EPIPE as an output error instead of dying silently
    crc_init();
    static Server server(dir);  // static: skip teardown of large containers at exit
    int rc = server.run();
    std::fflush(stderr);
    std::_Exit(rc);
}
