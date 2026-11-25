//! Property 3 (NOTES.md): values written through DuckDB read back equal, on every write path.
use ephyra::prelude::*;
use mssql_ephyra::driver::{crash, db, dsn, lit, words, Script};
use std::ops::ControlFlow;
use std::sync::Once;

#[derive(Clone, Debug, PartialEq)]
struct Rt {
    /// 0 CTAS (bulk), 1 CTAS (statements), 2 INSERT..SELECT (bulk), 3 INSERT..SELECT (statements), 4 INSERT VALUES
    mode: u8,
    types: Vec<String>,
    rows: Vec<Vec<Option<String>>>,
}

fn pool(ty: &str) -> Vec<&'static str> {
    match ty {
        "BOOLEAN" => vec!["true", "false"],
        "TINYINT" => vec!["-128", "127", "0", "1"],
        "SMALLINT" => vec!["-32768", "32767", "0", "-1"],
        "INTEGER" => vec!["-2147483648", "2147483647", "0", "42"],
        "BIGINT" => vec!["-9223372036854775808", "9223372036854775807", "0", "4294967296"],
        "UTINYINT" => vec!["0", "255", "128"],
        "USMALLINT" => vec!["0", "65535", "32768"],
        "UINTEGER" => vec!["0", "4294967295", "2147483648"],
        "UBIGINT" => vec!["0", "18446744073709551615", "9223372036854775808"],
        "HUGEINT" => vec!["0", "170141183460469231731687303715884105727", "-170141183460469231731687303715884105728", "99999999999999999999999999999999999999"],
        "FLOAT" => vec!["0", "1.5", "-1.5", "3.4028235e38", "1e-45", "0.1", "nan", "inf"],
        "DOUBLE" => vec!["0", "0.1", "1.7976931348623157e308", "1e100", "nan", "inf", "-inf", "2.2250738585072014e-308"],
        "VARCHAR" => vec![
            "", "a", " ", "a ", " a", "é", "ß", "日本語", "😀", "it's", "a\tb", "a\nb", "NULL", "@repeat('x', 4000)", "@repeat('x', 4001)",
            "@repeat('é', 4000)", "@repeat('x', 8001)", "@repeat('😀', 2001)", "@'a' || chr(0) || 'b'", "\u{feff}a", "\u{d7ff}\u{e000}\u{ffff}",
        ],
        "BLOB" => vec!["", "\\x00", "\\xFF\\x00\\x01", "abc", "@repeat('\\xAB', 8001)::BLOB", "@repeat('\\x00', 8000)::BLOB"],
        "DATE" => vec!["0001-01-01", "9999-12-31", "1970-01-01", "2024-02-29", "1752-12-31", "infinity", "-infinity", "0044-03-15 (BC)", "10000-01-01", "1582-10-10"],
        "TIME" => vec!["00:00:00", "23:59:59.999999", "12:34:56.123456", "12:34:56.1", "24:00:00"],
        "TIMESTAMP" => vec![
            "0001-01-01 00:00:00", "9999-12-31 23:59:59.999999", "1970-01-01 00:00:00", "2021-03-04 05:06:07.123456", "infinity", "-infinity",
            "1969-12-31 23:59:59.999999", "1753-01-01 00:00:00", "1899-12-31 23:59:59.5",
        ],
        "TIMESTAMP_S" => vec!["0001-01-01 00:00:00", "9999-12-31 23:59:59", "1969-12-31 23:59:59", "2021-03-04 05:06:07"],
        "TIMESTAMP_MS" => vec!["0001-01-01 00:00:00", "9999-12-31 23:59:59.999", "1969-12-31 23:59:59.999", "2021-03-04 05:06:07.123"],
        "TIMESTAMP_NS" => vec!["1677-09-22 00:00:00", "2262-04-11 23:47:16.8547758", "1969-12-31 23:59:59.9999999", "2021-03-04 05:06:07.1234567"],
        "TIMESTAMPTZ" => vec!["2021-03-04 05:06:07.123456+00", "1970-01-01 00:00:00+02", "0001-01-01 00:00:00+00", "9999-12-31 23:59:59.999999+00", "2021-06-01 12:00:00-11:30"],
        "UUID" => vec!["00000000-0000-0000-0000-000000000000", "6f9619ff-8b86-d011-b42d-00c04fc964ff", "ffffffff-ffff-ffff-ffff-ffffffffffff", "01020304-0506-0708-090a-0b0c0d0e0f10"],
        _ => {
            // DECIMAL(p,s)
            let (p, s) = dec_ps(ty);
            let nines = |n: usize| "9".repeat(n);
            let max = if s == 0 { nines(p) } else if p == s { format!("0.{}", nines(s)) } else { format!("{}.{}", nines(p - s), nines(s)) };
            let small = if s == 0 { "1".to_string() } else { format!("0.{}1", "0".repeat(s - 1)) };
            let v: Vec<String> = vec!["0".into(), max.clone(), format!("-{max}"), small.clone(), format!("-{small}")];
            v.into_iter().map(|s| &*Box::leak(s.into_boxed_str())).collect()
        },
    }
}

fn dec_ps(ty: &str) -> (usize, usize) {
    let inner = ty.trim_start_matches("DECIMAL(").trim_end_matches(')');
    let (p, s) = inner.split_once(',').unwrap();
    (p.parse().unwrap(), s.parse().unwrap())
}

/// Values SQL Server cannot hold; the extension refuses them (CLAUDE.md, filter pushdown / BCP encoders).
const HAZARD: &[&str] = &[
    "nan", "inf", "-inf", "infinity", "-infinity", "0044-03-15 (BC)", "10000-01-01", "24:00:00",
    "170141183460469231731687303715884105727", "-170141183460469231731687303715884105728",
];

const TYPES: &[&str] = &[
    "BOOLEAN", "TINYINT", "SMALLINT", "INTEGER", "BIGINT", "UTINYINT", "USMALLINT", "UINTEGER", "UBIGINT", "HUGEINT", "FLOAT", "DOUBLE", "VARCHAR", "VARCHAR",
    "VARCHAR", "BLOB", "DATE", "DATE", "TIME", "TIMESTAMP", "TIMESTAMP", "TIMESTAMP_S", "TIMESTAMP_MS", "TIMESTAMP_NS", "TIMESTAMPTZ", "UUID", "DEC", "DEC",
];

fn rt(g: &mut Source) -> Rt {
    let n = g.int(1usize..=5);
    let types: Vec<String> = (0..n)
        .map(|_| {
            let t = *g.pick(TYPES);
            if t == "DEC" {
                let p = *g.pick(&[1usize, 4, 9, 10, 18, 19, 28, 38]);
                let s = *g.pick(&[0, 1, p / 2, p]);
                format!("DECIMAL({p},{})", s.min(p))
            } else {
                t.to_string()
            }
        })
        .collect();
    let nrows = if g.bool_with(0.1) { g.int(0usize..=1) } else { g.int(2usize..=8) };
    let rows = (0..nrows)
        .map(|_| {
            types
                .iter()
                .map(|t| {
                    if g.bool_with(0.15) {
                        return None;
                    }
                    // values the extension refuses by name stay rare: a refused write compares nothing
                    let (hazard, safe): (Vec<&str>, Vec<&str>) = pool(t).into_iter().partition(|v| HAZARD.contains(v));
                    let p = if !hazard.is_empty() && g.bool_with(0.04) { hazard } else { safe };
                    Some(p[g.index(p.len())].to_string())
                })
                .collect()
        })
        .collect();
    Rt { mode: g.int(0u8..=4), types, rows }
}

fn shrink_rt(c: &Rt, visit: &mut dyn FnMut(Rt) -> ControlFlow<()>) -> ControlFlow<()> {
    for i in 0..c.rows.len() {
        let mut n = c.clone();
        n.rows.remove(i);
        visit(n)?;
    }
    if c.types.len() > 1 {
        for i in 0..c.types.len() {
            let mut n = c.clone();
            n.types.remove(i);
            for r in &mut n.rows {
                r.remove(i);
            }
            visit(n)?;
        }
    }
    for (ri, r) in c.rows.iter().enumerate() {
        for ci in 0..r.len() {
            if r[ci].is_some() {
                let mut n = c.clone();
                n.rows[ri][ci] = None;
                visit(n)?;
            }
        }
    }
    ControlFlow::Continue(())
}

fn cases() -> impl Gen<Value = Rt> {
    (|g: &mut Source| rt(g)).with_shrink(shrink_rt)
}

fn val(v: &Option<String>, ty: &str) -> String {
    match v {
        None => "NULL".into(),
        Some(v) => match v.strip_prefix('@') {
            Some(e) => format!("CAST({e} AS {ty})"),
            None => format!("CAST({} AS {ty})", lit(v)),
        },
    }
}

static INIT: Once = Once::new();

fn fail(sig: String, detail: String) -> ! {
    ephyra::signature(sig.clone());
    let dir = std::path::Path::new(".ephyra/findings");
    let _ = std::fs::create_dir_all(dir);
    let name: String = sig.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).take(120).collect();
    let _ = std::fs::write(dir.join(format!("{name}.txt")), format!("{sig}\n{detail}"));
    panic!("{sig}");
}

fn check(c: &Rt) {
    INIT.call_once(|| {
        let mut s = Script::new();
        s.stmt(&format!("ATTACH {} AS m (TYPE mssql)", lit(&dsn("master"))));
        s.stmt(&format!("SELECT mssql_exec('m', 'IF DB_ID(''{0}'') IS NULL CREATE DATABASE {0}')", db()));
        s.run();
    });
    let cols: Vec<String> = c.types.iter().enumerate().map(|(i, t)| format!("c{i} {t}")).collect();
    let ddl = format!("(k INTEGER, {})", cols.join(", "));
    let rows: Vec<String> = c
        .rows
        .iter()
        .enumerate()
        .map(|(k, r)| format!("({k}, {})", r.iter().zip(&c.types).map(|(v, t)| val(v, t)).collect::<Vec<_>>().join(", ")))
        .collect();
    let values = if rows.is_empty() { None } else { Some(format!("VALUES {}", rows.join(", "))) };
    let mut s = Script::new();
    s.stmt(&format!("ATTACH {} AS a (TYPE mssql)", lit(&dsn(&db()))));
    s.stmt("ATTACH ':memory:' AS l");
    s.stmt("SELECT mssql_exec('a', 'DROP TABLE IF EXISTS dbo.r')");
    s.stmt(&format!("CREATE TABLE l.src {ddl}"));
    // the source is filled in its own section: a literal DuckDB itself refuses is not a case
    let fill = values.as_ref().map(|v| s.section(&format!("INSERT INTO l.src {v}")));
    let mut writes = Vec::new();
    match c.mode {
        0 => writes.push(s.section("CREATE TABLE a.dbo.r AS SELECT * FROM l.src")),
        1 => {
            s.stmt("SET mssql_ctas_use_bcp = false");
            writes.push(s.section("CREATE TABLE a.dbo.r AS SELECT * FROM l.src"));
        },
        m => {
            writes.push(s.section(&format!("CREATE TABLE a.dbo.r {ddl}")));
            if m == 2 {
                s.stmt("SET mssql_insert_bcp_threshold = 1");
            } else {
                s.stmt("SET mssql_insert_use_bcp = false");
            }
            if m == 4 {
                if let Some(v) = &values {
                    writes.push(s.section(&format!("INSERT INTO a.dbo.r {v}")));
                }
            } else {
                writes.push(s.section("INSERT INTO a.dbo.r SELECT * FROM l.src"));
            }
        },
    }
    let want = s.section("SELECT * FROM l.src");
    let have = s.section("SELECT * FROM a.dbo.r");
    let out = s.run();
    if let Some(sig) = crash(&out) {
        fail(sig, format!("{:#?}\n{}", out.pre.errs, out.script));
    }
    if let Some(f) = fill {
        if !out.sections[f].errs.is_empty() {
            ephyra::classify(true, "source refused");
            return;
        }
    }
    for w in &writes {
        if let Some((ty, msg)) = out.sections[*w].errs.first() {
            // a refusal is allowed; keep one example of each wording to review
            let label = format!("refused:{}", words(msg, 7));
            let dir = std::path::Path::new(".ephyra/refusals");
            let _ = std::fs::create_dir_all(dir);
            let _ = std::fs::write(dir.join(format!("{label}.txt")), format!("{ty}: {msg}\n{:?}\n{}", c, out.script));
            ephyra::classify(true, "refused");
            return;
        }
    }
    if let Some((_, msg)) = out.sections[have].errs.first() {
        fail(format!("read:{}:{}", c.mode, words(msg, 6)), format!("{msg}\n{}", out.script));
    }
    let (mut w, mut h) = (out.sections[want].rows.clone(), out.sections[have].rows.clone());
    w.sort();
    h.sort();
    if w != h {
        // name the first column that differs
        let mut col = "rowcount".to_string();
        'f: for (a, b) in w.iter().zip(&h) {
            let (a, b): (serde_json::Value, serde_json::Value) =
                (serde_json::from_str(a).unwrap_or_default(), serde_json::from_str(b).unwrap_or_default());
            for (i, t) in c.types.iter().enumerate() {
                if a.get(format!("c{i}")) != b.get(format!("c{i}")) {
                    col = t.split('(').next().unwrap().to_string();
                    break 'f;
                }
            }
        }
        fail(format!("roundtrip:mode{}:{col}", c.mode), format!("want: {w:#?}\nhave: {h:#?}\n{}", out.script));
    }
    ephyra::classify(true, "compared");
}

#[ephyra::test(gen = cases(), tests = 200, failures = 20)]
fn roundtrip(c: Rt) {
    check(&c);
}
