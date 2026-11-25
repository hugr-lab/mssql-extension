use ephyra::prelude::*;
use mssql_ephyra::driver::{self, crash, db, dsn, lit, words, Script};
use mssql_ephyra::*;
use std::sync::Once;

static INIT: Once = Once::new();

fn init() {
    INIT.call_once(|| {
        let mut s = Script::new();
        s.stmt(&format!("ATTACH {} AS m (TYPE mssql)", lit(&dsn("master"))));
        s.stmt(&format!("SELECT mssql_exec('m', 'IF DB_ID(''{0}'') IS NULL CREATE DATABASE {0}')", db()));
        let out = s.run();
        assert!(out.pre.errs.is_empty(), "cannot create the test database: {:?}", out.pre.errs);
    });
}

fn cases() -> impl Gen<Value = Case> {
    (|g: &mut Source| case(g)).with_shrink(shrink_case)
}

const VARIANTS: [(&str, &str); 4] = [("l", "l."), ("a", "a.dbo."), ("b", "b.dbo."), ("o", "o.dbo.")];

fn fail(sig: String, detail: String) -> ! {
    ephyra::signature(sig.clone());
    // the last write for a signature is its shrunk case
    let dir = std::path::Path::new(".ephyra/findings");
    let _ = std::fs::create_dir_all(dir);
    let name: String = sig.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).take(120).collect();
    let _ = std::fs::write(dir.join(format!("{name}.txt")), format!("{sig}\n{detail}"));
    panic!("{sig}");
}

fn check_case(c: &Case) {
    init();
    let mut s = Script::new();
    s.stmt(&format!("ATTACH {} AS a (TYPE mssql)", lit(&dsn(&db()))));
    s.stmt(&format!("ATTACH {} AS b (TYPE mssql, remote_pushdown false)", lit(&dsn(&db()))));
    s.stmt(&format!("ATTACH {} AS o (TYPE mssql, remote_pushdown false, order_pushdown true)", lit(&dsn(&db()))));
    s.stmt("ATTACH ':memory:' AS l");
    s.stmt("SELECT mssql_exec('a', 'DROP TABLE IF EXISTS dbo.t0, dbo.t1, dbo.t2')");
    s.stmt(&format!("SELECT mssql_exec('a', {})", lit(&c.setup_tsql())));
    for i in 0..c.tables.len() {
        s.stmt(&format!("CREATE TABLE l.t{i} AS SELECT * FROM b.dbo.t{i}"));
    }
    // per query: (describe, rows) per variant
    let mut idx = Vec::new();
    let mut plans = Vec::new();
    for q in &c.queries {
        plans.push(s.section(&format!("EXPLAIN {}", q.sql("a.dbo."))));
        let mut v = Vec::new();
        for (_, prefix) in VARIANTS {
            let sql = q.sql(prefix);
            v.push((s.section(&format!("DESCRIBE {sql}")), s.section(&sql)));
        }
        idx.push(v);
    }
    let out = s.run();
    if std::env::var("EPH_DUMP").is_ok() {
        eprintln!("{}\n{:#?}", out.script, out.sections);
    }
    if let Some(sig) = crash(&out) {
        fail(sig, format!("{:#?}\n{}", out.pre.errs, out.script));
    }
    if !out.pre.errs.is_empty() {
        fail(format!("setup:{}", words(&out.pre.errs[0].1, 6)), format!("{:?}\n{}", out.pre.errs, out.script));
    }
    for (qi, q) in c.queries.iter().enumerate() {
        let plan = out.sections[plans[qi]].rows.join(" ");
        let whole = out.sections[plans[qi]].rows.first().is_some_and(|l| l.starts_with("MSSQL_SCAN"));
        ephyra::classify(whole, "pushed whole");
        ephyra::classify(!whole && plan.contains("MSSQL_SCAN"), "pushed part");
        let (ld, lr) = idx[qi][0];
        let reference = &out.sections[lr];
        for (vi, (name, prefix)) in VARIANTS.iter().enumerate().skip(1) {
            let (d, r) = idx[qi][vi];
            let got = &out.sections[r];
            let sql = q.sql(prefix);
            match (reference.errs.first(), got.errs.first()) {
                (Some((_, m)), Some(_)) => {
                    ephyra::classify(true, "both_err");
                    if std::env::var("EPH_SHOW_ERR").is_ok() {
                        eprintln!("BOTH_ERR {m:.200} :: {sql:.300}");
                    }
                    continue;
                },
                (None, Some((ty, msg))) => {
                    fail(format!("err:{name}:only:{ty}:{}", words(msg, 6)), format!("{msg}\n{sql}\nreference rows: {:?}\n{}", reference.rows, out.script))
                },
                (Some((ty, msg)), None) => {
                    fail(format!("err:{name}:missing:{ty}:{}", words(msg, 6)), format!("reference fails: {msg}\n{sql}\ngot rows: {:?}\n{}", got.rows, out.script))
                },
                (None, None) => ephyra::classify(true, "compared"),
            }
            ephyra::classify(got.rows.len() > 1, "rows>1");
            let (mut want, mut have) = (reference.rows.clone(), got.rows.clone());
            if q.order.is_none() {
                want.sort();
                have.sort();
            }
            if want != have {
                fail(format!("rows:{name}"), format!("{sql}\n  want ({}): {want:?}\n  have ({}): {have:?}\n{}", want.len(), have.len(), out.script));
            }
            if out.sections[ld].rows != out.sections[d].rows {
                fail(
                    format!("schema:{name}"),
                    format!("{sql}\n  want: {:?}\n  have: {:?}\n{}", out.sections[ld].rows, out.sections[d].rows, out.script),
                );
            }
        }
    }
}

#[ephyra::test(gen = cases(), tests = 100, failures = 12)]
fn qdiff(c: Case) {
    check_case(&c);
}

#[test]
fn generator_is_valid() {
    ephyra::check_generator(&Config::default().tests(2000), cases(), valid_case).assert_passed();
    let _ = driver::db();
}
