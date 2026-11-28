# Spec 094 experiments for the review of #622 (SQL Server 2025, throwaway container, 2026-11-24)

Script: `scripts/x1b_x2b.sh` (with `scripts/x2b_x3b.sql`). A throwaway 2025 container reached from DuckDB through
`TYPE mssql` (the built extension). X1b's vectors are written by a CTAS, `FLOAT[n]` to `vector(n)` by their
bytes, so both sides start from the same float32 values. Run three times, each time with new random vectors:
the numbers below give every run. The server's three distances are float32 values widened, in every row of
the third run (the first two checked euclidean only).

## X1b: VECTOR_DISTANCE on random float32 vectors

2000 vectors and one query vector, every component `random() * 2 - 1` as a FLOAT (not dyadic: X1's
multiples of 1/64 made every product and sum exact). Compared per row:
- `VECTOR_DISTANCE('euclidean')` with `array_distance`;
- `'dot'` with `array_negative_inner_product`;
- `'cosine'` with `array_cosine_distance`.

The server's `float` is a widened float32 in every row.

| dims | euclidean bit-equal | dot bit-equal | cosine bit-equal | euclidean max rel. error | dot max abs. error | cosine max abs. error |
|---|---|---|---|---|---|---|
| 384 | 430, 528, 498 / 2000 | 166, 149, 170 / 2000 | 1610, 1645, 1577 / 2000 | 4.8e-7, 5.9e-7, 4.9e-7 | 1.1e-5 (each run) | 1.2e-7 (each run) |
| 1536 | 278, 235, 283 / 2000 | 77, 68, 78 / 2000 | 1638, 1633, 1626 / 2000 | 1.1e-6, 9.3e-7, 1.1e-6 | 5.3e-5, 5.7e-5, 5.7e-5 | 1.2e-7 (each run) |

The rows ordered by each distance (ties by id), second and third runs:

| dims | metric | rank disagreements (of 2000) | top-10 shared | top-100 shared |
|---|---|---|---|---|
| 384 | euclidean | 8, 9 | 10, 10 | 100, 100 |
| 384 | dot | 0, 0 | 10, 10 | 100, 100 |
| 384 | cosine | 0, 0 | 10, 10 | 100, 100 |
| 1536 | euclidean | 24, 32 | 10, 10 | 100, 100 |
| 1536 | dot | 2, 2 | 10, 10 | 100, 100 |
| 1536 | cosine | 0, 0 | 10, 10 | 100, 100 |

"Rank disagreements" counts rows whose position differs, not swaps. The first run's ranks: euclidean 6 and 20
disagreements, dot 0 and 0, cosine 4 and 0. Its top-10 query was wrong, so only the second and third runs measured the top-k sets.

So none of the three is exact on real vectors: the server sums in another order, or in other lanes, than
DuckDB. The errors:
- euclidean: 5-9 float32 ulps;
- dot: about 15-30 ulps at the typical magnitudes (6-40), with no relative bound near 0;
- cosine: within an ulp or two of values near 1.0.

The order differs only between distances that close. X1's "bit-equal" for euclidean and dot was its data's,
not the functions'.

## X2b: `&&` against a non-rectangular constant; mixed SRIDs

The `&&` case uses `dbo.geo2`: a 21 x 21 grid of points over the box of `LINESTRING(100 100, 300 300)`.

| form | rows |
|---|---|
| DuckDB `g && 'LINESTRING(100 100, 300 300)'::GEOMETRY` | 441 |
| `g.STEnvelope().STIntersects(@line) = 1` (the draft's D3 form) | 21 |
| `g.STEnvelope().STIntersects(@line.STEnvelope()) = 1` | 441 |
| `g.Filter(@line.STEnvelope()) = 1` | 441 |

The SRID case uses `dbo.geosrid`: 100000 points of SRID 0 and 1000 of SRID 4326 on 0..1000, with a spatial index. The box (100..300)² is given in SRID 0, and the true count inside it is 4048, 43 of them SRID 4326.

| form | rows | 4326 rows | spatial index |
|---|---|---|---|
| DuckDB `g && box` (the SRID is not read, spec 093 DQ-45) | 4048 | -- | -- |
| `g.STIntersects(@box) = 1` | 4005 | 0 (all 1000 NULL) | yes |
| `g.STIntersects(geometry::STGeomFromWKB(@box.STAsBinary(), g.STSrid)) = 1` | 4048 | 43 | no |
| `g.STIntersects(@box) = 1 OR g.STSrid <> 0` | 5005 | 1000 | no |
| `g.Filter(@box) = 1 OR g.STSrid <> 0` | -- | -- | no |

A method over instances of two SRIDs returns NULL, and the row is dropped. The index serves a constant of the
rows' SRID only, and an OR with any other condition loses it.

Caveats:
- The index check used literal constants; the parameterised `@wkb` form's plan is X4's.
- `dbo.geo2` is points only and has no spatial index, so its `Filter` row was evaluated exactly. It does not
  show that `Filter` against an envelope is a superset of `&&` for other shapes, and it is not one: the line
  (0,0)-(10,10) against the box [8,20] x [0,2].

## X3b: geography, points the two models tell apart

The case uses `dbo.geog2`, five points: (70.03 N, 15 E), (71.05 N, 15 E), (70.5 N, 15 E), (69.9 N, 15 E) and
(70.9 N, 20 E). The box is 10..20 E, 70..71 N. Its geodesic southern edge bows north of 70 N at 15 E, and its
northern edge north of 71 N.

| form | ids |
|---|---|
| `g.STIntersects(@geography_box) = 1` (geodesic) | 2, 3 |
| DuckDB `g && box::GEOMETRY('OGC:CRS84')` | 1, 3, 5 |
| `geometry::STGeomFromWKB(g.STAsBinary(), 4326).STIntersects(@geometry_box) = 1` (planar on the server) | 1, 3, 5 |
| the same, envelope against envelope | 1, 3, 5 |

The geodesic answer differs both ways: id 1 is in the planar box only, and id 2 is in the geodesic one only. A geography's WKB is
longitude / latitude, as DuckDB's `OGC:CRS84` is, so the planar form on the server is DuckDB's answer.
