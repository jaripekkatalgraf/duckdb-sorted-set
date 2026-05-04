#define DUCKDB_EXTENSION_MAIN

#include "sorted_set_extension.hpp"
#include "sorted_set_type.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"      // ← ListVector lives here
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: read/write DuckDB list values
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<int32_t> read_int_list(const Value &v) {
    if (v.IsNull()) return {};
    std::vector<int32_t> out;
    for (const auto &child : ListValue::GetChildren(v))
        if (!child.IsNull())
            out.push_back(child.GetValue<int32_t>());
    return out;
}

static Value write_int_list(const int32_t *data, int32_t n) {
    vector<Value> children;
    children.reserve(n);
    for (int32_t i = 0; i < n; ++i)
        children.emplace_back(Value::INTEGER(data[i]));
    return Value::LIST(LogicalType::INTEGER, std::move(children));
}

static Value write_int_list(const std::vector<int32_t> &v) {
    return write_int_list(v.data(), (int32_t)v.size());
}


// ─────────────────────────────────────────────────────────────────────────────
// Macros for binary functions
// ─────────────────────────────────────────────────────────────────────────────

using BinarySetOp = int32_t(*)(
    const int32_t*, int32_t,
    const int32_t*, int32_t,
    int32_t*);

using BinaryPred = bool(*)(const int32_t*, int32_t, const int32_t*, int32_t);

// ─────────────────────────────────────────────────────────────────────────────
// Simple but reliable binary operations (REVERTED)
// ─────────────────────────────────────────────────────────────────────────────

static void ExecuteBinarySetOp(
    DataChunk &args, ExpressionState &, Vector &result,
    BinarySetOp op_func)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);

    for (idx_t i = 0; i < count; ++i) {
        if (args.data[0].GetValue(i).IsNull() || 
            args.data[1].GetValue(i).IsNull()) {
            FlatVector::SetNull(result, i, true);
            continue;
        }

        auto a = read_int_list(args.data[0].GetValue(i));
        auto b = read_int_list(args.data[1].GetValue(i));

        std::vector<int32_t> out(a.size() + b.size());
        int32_t n = op_func(a.data(), (int32_t)a.size(),
                            b.data(), (int32_t)b.size(),
                            out.data());

        out.resize(n);
        result.SetValue(i, write_int_list(out));
    }
}

static void ExecuteBinaryPredicate(
    DataChunk &args, ExpressionState &, Vector &result,
    BinaryPred pred_func)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);

    for (idx_t i = 0; i < count; ++i) {
        if (args.data[0].GetValue(i).IsNull() || 
            args.data[1].GetValue(i).IsNull()) {
            FlatVector::SetNull(result, i, true);
            continue;
        }

        auto a = read_int_list(args.data[0].GetValue(i));
        auto b = read_int_list(args.data[1].GetValue(i));

        bool res = pred_func(a.data(), (int32_t)a.size(),
                             b.data(), (int32_t)b.size());

        FlatVector::GetData<bool>(result)[i] = res;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Function implementations (unchanged except ctor)
// ─────────────────────────────────────────────────────────────────────────────

static void sorted_set_ctor(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto v = args.data[0].GetValue(i);
        if (v.IsNull()) {
            FlatVector::SetNull(result, i, true);
            continue;
        }
        auto data = read_int_list(v);
        if (!data.empty()) {
            int32_t new_n = sorted_set::normalise(data.data(), (int32_t)data.size(), data.data());
            data.resize(new_n);
        }
        result.SetValue(i, write_int_list(data));
    }
}

// ... [all your other function implementations stay exactly the same] ...

//
// set_is_valid(arr INTEGER[]) → BOOLEAN
// Returns true if the array is already sorted and unique (a valid sorted set).
//
static void set_is_valid(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto v = args.data[0].GetValue(i);
        if (v.IsNull()) { FlatVector::SetNull(result, i, true); continue; }
        auto data = read_int_list(v);
        result.SetValue(i, Value::BOOLEAN(
            sorted_set::is_sorted_unique(data.data(), (int32_t)data.size())));
    }
}

//
// set_contains(set INTEGER[], element INTEGER) → BOOLEAN
// Binary search. O(log n).
//
static void set_contains(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto sv = args.data[0].GetValue(i);
        auto ev = args.data[1].GetValue(i);
        if (sv.IsNull() || ev.IsNull()) {
            FlatVector::SetNull(result, i, true); continue;
        }
        auto data = read_int_list(sv);
        int32_t elem = ev.GetValue<int32_t>();
        result.SetValue(i, Value::BOOLEAN(
            sorted_set::contains(data.data(), (int32_t)data.size(), elem)));
    }
}

//
// set_intersect_size(a INTEGER[], b INTEGER[]) → INTEGER
// Size of the intersection without materialising it. O(n+m).
// Useful in HAVING clauses: HAVING set_intersect_size(px, $s) = cardinality($s)
//
static void set_intersect_size(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto av = args.data[0].GetValue(i);
        auto bv = args.data[1].GetValue(i);
        if (av.IsNull() || bv.IsNull()) {
            FlatVector::SetNull(result, i, true); continue;
        }
        auto a = read_int_list(av);
        auto b = read_int_list(bv);
        result.SetValue(i, Value::INTEGER(
            sorted_set::intersect_size(
                a.data(), (int32_t)a.size(),
                b.data(), (int32_t)b.size())));
    }
}

//
// set_size(set INTEGER[]) → INTEGER
// Alias for cardinality() / array_length() — consistent naming.
//
static void set_size(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto v = args.data[0].GetValue(i);
        if (v.IsNull()) { FlatVector::SetNull(result, i, true); continue; }
        auto data = read_int_list(v);
        result.SetValue(i, Value::INTEGER((int32_t)data.size()));
    }
}

static void set_min(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto v = args.data[0].GetValue(i);
        if (v.IsNull()) { 
            FlatVector::SetNull(result, i, true); 
            continue; 
        }
        auto data = read_int_list(v);
        if (data.empty()) { 
            FlatVector::SetNull(result, i, true); 
            continue; 
        }
        auto min_it = std::min_element(data.begin(), data.end());
        result.SetValue(i, Value::INTEGER(*min_it));
    }
}

static void set_max(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto v = args.data[0].GetValue(i);
        if (v.IsNull()) { 
            FlatVector::SetNull(result, i, true); 
            continue; 
        }
        auto data = read_int_list(v);
        if (data.empty()) { 
            FlatVector::SetNull(result, i, true); 
            continue; 
        }
        auto max_it = std::max_element(data.begin(), data.end());
        result.SetValue(i, Value::INTEGER(*max_it));
    }
}

//
// set_rank(set INTEGER[], element INTEGER) → INTEGER
// 0-based rank of element in set (its position). -1 if not present. O(log n).
//
static void set_rank(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto sv = args.data[0].GetValue(i);
        auto ev = args.data[1].GetValue(i);
        if (sv.IsNull() || ev.IsNull()) {
            FlatVector::SetNull(result, i, true); continue;
        }
        auto data = read_int_list(sv);
        int32_t elem = ev.GetValue<int32_t>();
        auto it = std::lower_bound(data.begin(), data.end(), elem);
        if (it == data.end() || *it != elem)
            result.SetValue(i, Value::INTEGER(-1));
        else
            result.SetValue(i, Value::INTEGER((int32_t)(it - data.begin())));
    }
}

//
// set_at(set INTEGER[], rank INTEGER) → INTEGER
// Element at 0-based rank. NULL if out of bounds. O(1).
//
static void set_at(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto sv = args.data[0].GetValue(i);
        auto rv = args.data[1].GetValue(i);
        if (sv.IsNull() || rv.IsNull()) {
            FlatVector::SetNull(result, i, true); continue;
        }
        auto data = read_int_list(sv);
        int32_t rank = rv.GetValue<int32_t>();
        if (rank < 0 || rank >= (int32_t)data.size())
            FlatVector::SetNull(result, i, true);
        else
            result.SetValue(i, Value::INTEGER(data[rank]));
    }
}

//
// set_from_range(lo INTEGER, hi INTEGER) → INTEGER[]
// Construct {lo, lo+1, ..., hi}. Useful for testing and for encoding
// pixel ID ranges.
//
static void set_from_range(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto lv = args.data[0].GetValue(i);
        auto hv = args.data[1].GetValue(i);
        if (lv.IsNull() || hv.IsNull()) {
            FlatVector::SetNull(result, i, true); continue;
        }
        int32_t lo = lv.GetValue<int32_t>();
        int32_t hi = hv.GetValue<int32_t>();
        std::vector<int32_t> data;
        data.reserve(std::max(0, hi - lo + 1));
        for (int32_t v = lo; v <= hi; ++v) data.push_back(v);
        result.SetValue(i, write_int_list(data));
    }
}

//
// set_add(set INTEGER[], element INTEGER) → INTEGER[]
// Insert element if not already present. O(n) — shifts elements.
//
static void set_add(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto sv = args.data[0].GetValue(i);
        auto ev = args.data[1].GetValue(i);
        if (sv.IsNull() || ev.IsNull()) {
            FlatVector::SetNull(result, i, true); continue;
        }
        auto data = read_int_list(sv);
        int32_t elem = ev.GetValue<int32_t>();
        auto it = std::lower_bound(data.begin(), data.end(), elem);
        if (it == data.end() || *it != elem)
            data.insert(it, elem);
        result.SetValue(i, write_int_list(data));
    }
}

//
// set_remove(set INTEGER[], element INTEGER) → INTEGER[]
// Remove element if present. O(n).
//
static void set_remove(
    DataChunk &args, ExpressionState &, Vector &result)
{
    idx_t count = args.size();
    result.SetVectorType(VectorType::FLAT_VECTOR);
    for (idx_t i = 0; i < count; ++i) {
        auto sv = args.data[0].GetValue(i);
        auto ev = args.data[1].GetValue(i);
        if (sv.IsNull() || ev.IsNull()) {
            FlatVector::SetNull(result, i, true); continue;
        }
        auto data = read_int_list(sv);
        int32_t elem = ev.GetValue<int32_t>();
        auto it = std::lower_bound(data.begin(), data.end(), elem);
        if (it != data.end() && *it == elem) data.erase(it);
        result.SetValue(i, write_int_list(data));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Load function (NEW STYLE)
// ─────────────────────────────────────────────────────────────────────────────

static void LoadInternal(ExtensionLoader &loader) {

    // ── Constructor ─────────────────────────────────────────────────────
    loader.RegisterFunction(ScalarFunction(
        "sorted_set",
        {LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::LIST(LogicalType::INTEGER),
        sorted_set_ctor));

    loader.RegisterFunction(ScalarFunction(
        "set_from_range",
        {LogicalType::INTEGER, LogicalType::INTEGER},
        LogicalType::LIST(LogicalType::INTEGER),
        set_from_range));

    // ── Predicates ──────────────────────────────────────────────────────
    loader.RegisterFunction(ScalarFunction(
        "set_is_valid",
        {LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::BOOLEAN,
        set_is_valid));

    loader.RegisterFunction(ScalarFunction(
        "set_contains",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER},
        LogicalType::BOOLEAN,
        set_contains));

    loader.RegisterFunction(ScalarFunction(
        "set_equal",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::BOOLEAN,
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            ExecuteBinaryPredicate(args, state, result, sorted_set::equal);
        }));

    loader.RegisterFunction(ScalarFunction(
        "set_subset_of",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::BOOLEAN,
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            ExecuteBinaryPredicate(args, state, result, sorted_set::subset_of);
        }));

    loader.RegisterFunction(ScalarFunction(
        "set_superset_of",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::BOOLEAN,
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            ExecuteBinaryPredicate(args, state, result, sorted_set::superset_of);
        }));

    loader.RegisterFunction(ScalarFunction(
        "set_disjoint",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::BOOLEAN,
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            ExecuteBinaryPredicate(args, state, result, sorted_set::disjoint);
        }));

    // ── Set Operations ──────────────────────────────────────────────────
    loader.RegisterFunction(ScalarFunction(
        "set_intersect",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::LIST(LogicalType::INTEGER),
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            ExecuteBinarySetOp(args, state, result, sorted_set::intersect);
        }));

    loader.RegisterFunction(ScalarFunction(
        "set_union",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::LIST(LogicalType::INTEGER),
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            ExecuteBinarySetOp(args, state, result, sorted_set::union_);
        }));

    loader.RegisterFunction(ScalarFunction(
        "set_subtract",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::LIST(LogicalType::INTEGER),
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            ExecuteBinarySetOp(args, state, result, sorted_set::subtract);
        }));

    loader.RegisterFunction(ScalarFunction(
        "set_symmetric_diff",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
        LogicalType::LIST(LogicalType::INTEGER),
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            ExecuteBinarySetOp(args, state, result, sorted_set::symmetric_diff);
        }));

    // ── Scalar Accessors ────────────────────────────────────────────────
    loader.RegisterFunction(ScalarFunction("set_size",         {LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::INTEGER, set_size));
    loader.RegisterFunction(ScalarFunction("set_min",          {LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::INTEGER, set_min));
    loader.RegisterFunction(ScalarFunction("set_max",          {LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::INTEGER, set_max));
    loader.RegisterFunction(ScalarFunction("set_intersect_size",{LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::INTEGER, set_intersect_size));
    loader.RegisterFunction(ScalarFunction("set_rank",         {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER}, LogicalType::INTEGER, set_rank));
    loader.RegisterFunction(ScalarFunction("set_at",           {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER}, LogicalType::INTEGER, set_at));

    // ── Mutation ────────────────────────────────────────────────────────
    loader.RegisterFunction(ScalarFunction(
        "set_add",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER},
        LogicalType::LIST(LogicalType::INTEGER),
        set_add));

    loader.RegisterFunction(ScalarFunction(
        "set_remove",
        {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER},
        LogicalType::LIST(LogicalType::INTEGER),
        set_remove));
}

void SortedSetExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string SortedSetExtension::Name() {
    return "sorted_set";
}

std::string SortedSetExtension::Version() const {
    return "v0.1.0";
}

} // namespace duckdb

// ─────────────────────────────────────────────────────────────────────────────
// Extension entry point
// ─────────────────────────────────────────────────────────────────────────────

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(sorted_set, loader) {
    duckdb::LoadInternal(loader);
}

} // extern "C"