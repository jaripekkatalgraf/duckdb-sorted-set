# sorted_set

A DuckDB extension providing native sorted integer set operations with O(n+m) merge-based algorithms.

DuckDB's built-in `list_intersect`, `list_union`, and related functions operate on unsorted lists and use O(n×m) nested-loop algorithms. When your lists are known to be sorted and deduplicated — coverage arrays, pixel ID sets, document term lists — this extension provides the same operations with O(n+m) merge-based implementations that exploit the sorted invariant.

The extension uses the same `INTEGER[]` storage as DuckDB's standard list type, so sorted sets interoperate freely with all existing list functions.

## Installation

```bash
git clone --recurse-submodules https://github.com/yourname/sorted_set.git
cd sorted_set
GEN=ninja make
```

```sql
LOAD 'build/release/extension/sorted_set/sorted_set.duckdb_extension';
```

## Functions

### Construction

**`sorted_set(arr INTEGER[]) → INTEGER[]`**

Construct a valid sorted set from any integer array. Sorts and deduplicates in O(n log n). This is the constructor — call it once at insert time, then all subsequent operations are O(n+m).

```sql
SELECT sorted_set([3, 1, 2, 1, 3]::INTEGER[]);  -- → [1, 2, 3]
SELECT sorted_set([]);                            -- → []
```

**`set_from_range(lo INTEGER, hi INTEGER) → INTEGER[]`**

Construct the set {lo, lo+1, ..., hi}. Returns empty set if lo > hi.

```sql
SELECT set_from_range(0, 4);   -- → [0, 1, 2, 3, 4]
SELECT set_from_range(5, 3);   -- → []
```

**`set_is_valid(arr INTEGER[]) → BOOLEAN`**

Returns true if the array is already sorted and contains no duplicates. Useful for validating data at insert time.

```sql
SELECT set_is_valid([1, 2, 3]::INTEGER[]);  -- → true
SELECT set_is_valid([3, 1, 2]::INTEGER[]);  -- → false  (not sorted)
SELECT set_is_valid([1, 1, 2]::INTEGER[]);  -- → false  (duplicate)
```

---

### Predicates

All predicate functions return NULL if either input is NULL.

**`set_contains(s INTEGER[], x INTEGER) → BOOLEAN`**

Binary search. O(log n).

```sql
SELECT set_contains([1, 3, 5, 7]::INTEGER[], 3);  -- → true
SELECT set_contains([1, 3, 5, 7]::INTEGER[], 4);  -- → false
```

**`set_equal(a INTEGER[], b INTEGER[]) → BOOLEAN`**

True if both sets contain exactly the same elements. O(n) — memcmp after size check.

```sql
SELECT set_equal([1, 2, 3]::INTEGER[], [1, 2, 3]::INTEGER[]);  -- → true
SELECT set_equal([1, 2]::INTEGER[], [1, 2, 3]::INTEGER[]);     -- → false
```

**`set_subset_of(a INTEGER[], b INTEGER[]) → BOOLEAN`**

True if every element of `a` appears in `b` (a ⊆ b). O(n+m) merge walk — faster than repeated `set_contains` calls.

```sql
SELECT set_subset_of([1, 2]::INTEGER[], [1, 2, 3]::INTEGER[]);  -- → true
SELECT set_subset_of([1, 4]::INTEGER[], [1, 2, 3]::INTEGER[]);  -- → false
SELECT set_subset_of([]::INTEGER[], [1, 2, 3]::INTEGER[]);      -- → true
```

**`set_superset_of(a INTEGER[], b INTEGER[]) → BOOLEAN`**

True if `a` contains every element of `b` (a ⊇ b). Equivalent to `set_subset_of(b, a)`.

```sql
SELECT set_superset_of([1, 2, 3]::INTEGER[], [1, 2]::INTEGER[]);  -- → true
```

**`set_disjoint(a INTEGER[], b INTEGER[]) → BOOLEAN`**

True if `a` and `b` share no elements. O(n+m).

```sql
SELECT set_disjoint([1, 2]::INTEGER[], [3, 4]::INTEGER[]);     -- → true
SELECT set_disjoint([1, 2, 3]::INTEGER[], [3, 4]::INTEGER[]);  -- → false
```

---

### Set Operations

All operations return a new sorted set. Both inputs must be valid sorted sets (produced by `sorted_set()` or known-sorted data). Results are always sorted and deduplicated.

**`set_intersect(a INTEGER[], b INTEGER[]) → INTEGER[]`**

Elements in both a and b. O(n+m).

```sql
SELECT set_intersect([1,2,3,4]::INTEGER[], [2,4,6]::INTEGER[]);
-- → [2, 4]
```

**`set_union(a INTEGER[], b INTEGER[]) → INTEGER[]`**

Elements in either a or b. O(n+m).

```sql
SELECT set_union([1,3,5]::INTEGER[], [2,4,6]::INTEGER[]);
-- → [1, 2, 3, 4, 5, 6]
```

**`set_subtract(a INTEGER[], b INTEGER[]) → INTEGER[]`**

Elements in a but not in b (a \ b). O(n+m).

```sql
SELECT set_subtract([1,2,3,4]::INTEGER[], [2,4]::INTEGER[]);
-- → [1, 3]
```

**`set_symmetric_diff(a INTEGER[], b INTEGER[]) → INTEGER[]`**

Elements in exactly one of a or b. O(n+m).

```sql
SELECT set_symmetric_diff([1,2,3]::INTEGER[], [2,3,4]::INTEGER[]);
-- → [1, 4]
```

---

### Scalar Accessors

**`set_size(s INTEGER[]) → INTEGER`**

Number of elements. Equivalent to `cardinality(s)`.

**`set_min(s INTEGER[]) → INTEGER`**

Minimum element. O(1) — first element of sorted array. NULL for empty set.

**`set_max(s INTEGER[]) → INTEGER`**

Maximum element. O(1) — last element of sorted array. NULL for empty set.

**`set_intersect_size(a INTEGER[], b INTEGER[]) → INTEGER`**

Size of the intersection without materialising it. O(n+m). Use in `WHERE` and `HAVING` clauses to filter by overlap count without the allocation cost of `set_intersect`.

```sql
-- Features that share at least 3 pixels with target
SELECT feat_id FROM coverage
WHERE set_intersect_size(px, $target) >= 3;
```

**`set_rank(s INTEGER[], x INTEGER) → INTEGER`**

Zero-based position of `x` in `s`. Returns -1 if not present. O(log n).

```sql
SELECT set_rank([10, 20, 30, 40]::INTEGER[], 20);  -- → 1
SELECT set_rank([10, 20, 30, 40]::INTEGER[], 25);  -- → -1
```

**`set_at(s INTEGER[], rank INTEGER) → INTEGER`**

Element at zero-based rank. Returns NULL if rank is out of bounds. O(1).

```sql
SELECT set_at([10, 20, 30, 40]::INTEGER[], 2);   -- → 30
SELECT set_at([10, 20, 30, 40]::INTEGER[], 99);  -- → NULL
```

---

### Mutation

These return new sets — DuckDB values are immutable. Both are O(n) due to array shifting; for bulk modifications prefer `sorted_set(array_append(...))`.

**`set_add(s INTEGER[], x INTEGER) → INTEGER[]`**

Insert `x` at the correct sorted position. No-op if already present.

```sql
SELECT set_add([1, 3, 5]::INTEGER[], 4);  -- → [1, 3, 4, 5]
SELECT set_add([1, 3, 5]::INTEGER[], 3);  -- → [1, 3, 5]   (no-op)
```

**`set_remove(s INTEGER[], x INTEGER) → INTEGER[]`**

Remove `x` if present. No-op if absent.

```sql
SELECT set_remove([1, 2, 3, 4]::INTEGER[], 3);  -- → [1, 2, 4]
SELECT set_remove([1, 2, 3]::INTEGER[], 99);    -- → [1, 2, 3]
```

---

## Usage patterns

### Storing sorted sets in a table

Construct once at insert time using `sorted_set()`. All subsequent reads and operations are O(n+m).

```sql
CREATE TABLE feature_coverage (
    feat_id  INTEGER PRIMARY KEY,
    px       INTEGER[]    -- maintained as sorted set
);

-- Insert: normalise at write time
INSERT INTO feature_coverage
SELECT feat_id, sorted_set(list(pixel_id))
FROM raw_pixels
GROUP BY feat_id;
```

### Superset queries: which features cover all target pixels?

The core operation in predicate search — find features whose coverage is a superset of the target set S.

```sql
-- Replaces: WHERE list_has_all(px, $s)  (O(n*m) unsorted)
-- With:     WHERE set_subset_of($s, px)  (O(n+m) merge)

SELECT feat_id
FROM feature_coverage
WHERE set_subset_of($s::INTEGER[], px)    -- S ⊆ px
  AND NOT set_subset_of(px, $s::INTEGER[]) -- px ⊄ S (strict superset)
```

### Exact coverage queries

```sql
-- Features that cover exactly S
SELECT feat_id FROM feature_coverage
WHERE set_equal(px, $s::INTEGER[]);
```

### BFS intersection: carry coverage through recursive CTEs

Unlike unsorted list operations, `set_intersect` returns a value that can be passed through recursive CTEs and used as a deduplication key — the same intermediate intersection reached via different paths has the same sorted representation and thus the same dedup key.

```sql
WITH RECURSIVE
seed AS (
    SELECT feat_id, px FROM feature_coverage
    WHERE set_subset_of($s, px)
),
bfs(feat_ids, coverage, depth) AS (
    SELECT [feat_id], px, 1 FROM seed

    UNION ALL

    SELECT bfs.feat_ids || [seed.feat_id],
           set_intersect(bfs.coverage, seed.px),
           bfs.depth + 1
    FROM bfs
    JOIN seed ON seed.feat_id > bfs.feat_ids[-1]
    WHERE bfs.depth < 5
      AND set_size(bfs.coverage) > set_size($s)
      AND set_subset_of($s, seed.px)
)
SELECT feat_ids, depth
FROM bfs
WHERE set_equal(coverage, $s)
```

### Overlap count in HAVING clause

```sql
-- Features with at least 4 pixels in common with target
-- set_intersect_size avoids allocating the result array
SELECT feat_id, set_intersect_size(px, $target) AS overlap
FROM feature_coverage
WHERE set_intersect_size(px, $target) >= 4
ORDER BY overlap DESC;
```

### OR search: union of subsets

```sql
WITH
target AS (SELECT $s::INTEGER[] AS s),
subsets AS (
    SELECT feat_id, px FROM feature_coverage, target
    WHERE set_subset_of(px, target.s)
      AND set_size(px) > 0
)
-- Find pair whose union equals target
SELECT a.feat_id, b.feat_id
FROM subsets a
JOIN subsets b ON b.feat_id > a.feat_id, target
WHERE set_equal(set_union(a.px, b.px), target.s)
```

### Working with global pixel IDs

For multi-example systems where pixel IDs are encoded as `example_id * NB + local_id`:

```sql
-- Build global-ID coverage for a predicate across all examples
SELECT feat_id, sorted_set(list(example_id * 256 + pixel_id)) AS px
FROM pixels
WHERE c = 2
GROUP BY feat_id;

-- S ⊆ coverage across all examples simultaneously:
-- No per-example loop needed — the global ID encoding handles it
WHERE set_subset_of($s_global, px)
```

---

## Performance notes

**All merge operations are O(n+m)** where n and m are the sizes of the input sets. This is optimal — you must read every element of both inputs at least once.

**Compared to DuckDB built-ins:**
- `list_intersect` — O(n×m) with hash table overhead. `set_intersect` — O(n+m).
- `list_has_all` — O(n×m) nested loop. `set_subset_of` — O(n+m) merge walk.
- `list_sort` + `list_distinct` at query time — O(n log n) per operation. `sorted_set()` once at insert — O(n log n) at write, free at read.

**`set_intersect_size` vs `set_intersect`:** When you only need the count (e.g. `HAVING set_intersect_size(px, target) = 5`), `set_intersect_size` saves the allocation and list-building overhead of materialising the full result. Prefer it in filter predicates.

**The sorted invariant is not enforced at read time.** If you insert data directly without using `sorted_set()`, operations will produce incorrect results silently. Use `set_is_valid()` in an assertion or CHECK constraint if you need guarantees:

```sql
ALTER TABLE feature_coverage
ADD CONSTRAINT px_is_sorted CHECK (set_is_valid(px));
```

---

## Running the tests

```bash
make test
# or
./build/release/test/unittest --test-dir test [sql]
```

## License

MIT
