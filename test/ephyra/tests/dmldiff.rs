//! Property 4 (NOTES.md): a sequence of DML statements leaves a catalog table and a local
//! DuckDB copy with the same rows, on every DML path.
use ephyra::prelude::*;
use mssql_ephyra::driver::{crash, db, dsn, lit as sql_lit, words, Script};
use mssql_ephyra::*;
use std::ops::ControlFlow;
use std::sync::Once;

#[derive(Clone, Debug, PartialEq)]
enum Dml {
    Update { set: Vec<(String, E)>, from: Option<usize>, wher: Option<E>, ret: bool },
    Delete { using: Option<usize>, wher: Option<E>, ret: bool },
    InsertSelect { src: usize, items: Vec<E>, wher: Option<E>, ret: bool },
    InsertValues { rows: Vec<Vec<E>>, ret: bool },
    Merge { src: usize, cond: Option<E>, update: Option<Vec<(String, E)>>, delete: bool, insert: Option<Vec<E>> },
}

#[derive(Clone, Debug, PartialEq)]
struct DCase {
    tables: Vec<Table>,
    stmts: Vec<Dml>,
    /// 0 default, 1 dml_pushdown off, 2 + always staged, 3 + small batches, statements only
    variant: u8,
    txn: bool,
}

fn list(v: &[E], p: &str) -> String {
    v.iter().map(|e| e.sql(p)).collect::<Vec<_>>().join(", ")
}

fn sets(v: &[(String, E)], p: &str) -> String {
    v.iter().map(|(c, e)| format!("{c} = {}", e.sql(p))).collect::<Vec<_>>().join(", ")
}

impl Dml {
    fn sql(&self, p: &str) -> String {
        let r = |ret: &bool| if *ret { " RETURNING *" } else { "" };
        match self {
            Dml::Update { set, from, wher, ret } => {
                let mut s = format!("UPDATE {p}t0 AS x0 SET {}", sets(set, p));
                let mut conds = Vec::new();
                if let Some(j) = from {
                    s += &format!(" FROM {p}t{j} AS x1");
                    conds.push("x0.k = x1.k".to_string());
                }
                if let Some(w) = wher {
                    conds.push(w.sql(p));
                }
                if !conds.is_empty() {
                    s += &format!(" WHERE {}", conds.join(" AND "));
                }
                s + r(ret)
            },
            Dml::Delete { using, wher, ret } => {
                let mut s = format!("DELETE FROM {p}t0 AS x0");
                let mut conds = Vec::new();
                if let Some(j) = using {
                    s += &format!(" USING {p}t{j} AS x1");
                    conds.push("x0.k = x1.k".to_string());
                }
                if let Some(w) = wher {
                    conds.push(w.sql(p));
                }
                if !conds.is_empty() {
                    s += &format!(" WHERE {}", conds.join(" AND "));
                }
                s + r(ret)
            },
            Dml::InsertSelect { src, items, wher, ret } => {
                let mut s = format!("INSERT INTO {p}t0 SELECT {} FROM {p}t{src} AS x0", list(items, p));
                if let Some(w) = wher {
                    s += &format!(" WHERE {}", w.sql(p));
                }
                s + r(ret)
            },
            Dml::InsertValues { rows, ret } => {
                let rows: Vec<String> = rows.iter().map(|row| format!("({})", list(row, p))).collect();
                format!("INSERT INTO {p}t0 VALUES {}{}", rows.join(", "), r(ret))
            },
            Dml::Merge { src, cond, update, delete, insert } => {
                let mut s = format!("MERGE INTO {p}t0 AS x0 USING {p}t{src} AS x1 ON (x0.k = x1.k)");
                let c = cond.as_ref().map(|c| format!(" AND {}", c.sql(p))).unwrap_or_default();
                if let Some(u) = update {
                    s += &format!(" WHEN MATCHED{c} THEN UPDATE SET {}", sets(u, p));
                    if *delete {
                        s += " WHEN MATCHED THEN DELETE";
                    }
                } else if *delete {
                    s += &format!(" WHEN MATCHED{c} THEN DELETE");
                }
                if let Some(i) = insert {
                    s += &format!(" WHEN NOT MATCHED THEN INSERT VALUES ({})", list(i, p));
                }
                s
            },
        }
    }
}

fn dml_table(g: &mut Source) -> Table {
    let mut t = table(g);
    // `datetime` holds 1/300 s: a local TIMESTAMP column is no model for it
    for ci in 1..t.cols.len() {
        // binary(4) pads what is assigned: a local BLOB column is no model for it
        if matches!(t.cols[ci].ty, Ty::DateTime | Ty::Time7 | Ty::Dto7 | Ty::SmallDateTime | Ty::Binary) {
            t.cols[ci].ty = Ty::Int;
            for r in &mut t.rows {
                r[ci] = None;
            }
        }
    }
    // a varchar column on a code page turns what it cannot hold into '?' on the server (issue #225's
    // note in CLAUDE.md): keep every string inside CP1252
    for r in &mut t.rows {
        for v in r.iter_mut() {
            if v.as_deref() == Some("日本") {
                *v = None;
            }
        }
    }
    t
}

fn col_expr(g: &mut Source, sc: &mut Scope, ty: &Ty) -> E {
    let c = ty.class();
    let depth = if c == Class::Str { 0 } else { g.int(0usize..=2) };
    expr(g, sc, c, depth)
}

fn assignments(g: &mut Source, sc: &mut Scope, target: &Table) -> Vec<(String, E)> {
    assignments_with(g, sc, target, false)
}

/// `merge`: a boolean expression assigned to a bit column in a MERGE is BUG 1 (NOTES.md): the
/// statement fails with a server syntax error. Only a column or a constant is generated there.
fn assignments_with(g: &mut Source, sc: &mut Scope, target: &Table, merge: bool) -> Vec<(String, E)> {
    let n = g.int(1usize..=2.min(target.cols.len() - 1));
    let mut cols: Vec<usize> = (1..target.cols.len()).collect();
    let mut out = Vec::new();
    for _ in 0..n {
        let c = cols.remove(g.index(cols.len()));
        let e = if merge && target.cols[c].ty == Ty::Bit { expr(g, sc, Class::Bool, 0) } else { col_expr(g, sc, &target.cols[c].ty) };
        out.push((target.cols[c].name.clone(), e));
    }
    out
}

fn dml(g: &mut Source, tables: &[Table], i: usize) -> Dml {
    let target = &tables[0];
    let other = g.index(tables.len());
    let one = [From { table: 0, join: "", on: None }];
    let two = [From { table: 0, join: "", on: None }, From { table: other, join: "JOIN", on: None }];
    let scope = |from: &[From], sub: bool| Scope { cols: scope_of(tables, from, 0), tables, next_alias: 2, allow_sub: sub, pushable: false };
    let cond = |g: &mut Source, sc: &mut Scope, p: f64| if g.bool_with(p) { Some(expr(g, sc, Class::Bool, 2)) } else { None };
    match g.int(0u32..=9) {
        0..=2 => {
            let joined = g.bool_with(0.3);
            let mut sc = if joined { scope(&two, false) } else { scope(&one, true) };
            let wher = cond(g, &mut sc, 0.8);
            sc.allow_sub = false;
            Dml::Update { set: assignments(g, &mut sc, target), from: joined.then_some(other), wher, ret: g.bool_with(0.2) }
        },
        3 | 4 => {
            let joined = g.bool_with(0.3);
            let mut sc = if joined { scope(&two, false) } else { scope(&one, true) };
            Dml::Delete { using: joined.then_some(other), wher: cond(g, &mut sc, 0.9), ret: g.bool_with(0.2) }
        },
        5 | 6 => {
            let from = [From { table: other, join: "", on: None }];
            let mut sc = scope(&from, false);
            let mut items = vec![E::Bin("+", Box::new(E::Col("x0.k".into())), Box::new(E::Lit(format!("{}", 100 * 10usize.pow(i as u32)))))];
            for c in &target.cols[1..] {
                items.push(col_expr(g, &mut sc, &c.ty));
            }
            Dml::InsertSelect { src: other, items, wher: cond(g, &mut sc, 0.4), ret: g.bool_with(0.2) }
        },
        7 => {
            let n = g.int(1usize..=3);
            let rows = (0..n)
                .map(|r| {
                    let mut row = vec![E::Lit(format!("{}", 5_000_000 * (i + 1) + r))];
                    for c in &target.cols[1..] {
                        row.push(lit(g, c.ty.class()));
                    }
                    row
                })
                .collect();
            Dml::InsertValues { rows, ret: g.bool_with(0.2) }
        },
        _ => {
            let mut sc = scope(&two, false);
            let c = cond(g, &mut sc, 0.4);
            let update = if g.bool_with(0.6) { Some(assignments_with(g, &mut sc, target, true)) } else { None };
            let delete = update.is_none() || (c.is_some() && g.bool_with(0.3));
            let insert = if g.bool_with(0.6) {
                // the source side only
                let from = [From { table: 0, join: "", on: None }, From { table: other, join: "JOIN", on: None }];
                let cols: Vec<_> = scope_of(tables, &from, 0).into_iter().filter(|(n, _)| n.starts_with("x1.")).collect();
                let mut s1 = Scope { cols, tables, next_alias: 2, allow_sub: false, pushable: false };
                let mut v = vec![E::Col("x1.k".into())];
                for c in &target.cols[1..] {
                    v.push(if c.ty == Ty::Bit { expr(g, &mut s1, Class::Bool, 0) } else { col_expr(g, &mut s1, &c.ty) });
                }
                Some(v)
            } else {
                None
            };
            Dml::Merge { src: other, cond: c, update, delete, insert }
        },
    }
}

fn dcase(g: &mut Source) -> DCase {
    let nt = g.int(1usize..=2);
    let tables: Vec<Table> = (0..nt).map(|_| dml_table(g)).collect();
    let n = g.int(1usize..=4);
    let stmts = (0..n).map(|i| dml(g, &tables, i)).collect();
    DCase { tables, stmts, variant: g.int(0u8..=3), txn: g.bool_with(0.25) }
}

fn shrink_dml(d: &Dml, visit: Visit<Dml>) -> ControlFlow<()> {
    match d {
        Dml::Update { set, from, wher, ret } => {
            if *ret {
                visit(Dml::Update { set: set.clone(), from: *from, wher: wher.clone(), ret: false })?;
            }
            if set.len() > 1 {
                for i in 0..set.len() {
                    let mut s = set.clone();
                    s.remove(i);
                    visit(Dml::Update { set: s, from: *from, wher: wher.clone(), ret: *ret })?;
                }
            }
            shrink_opt(wher, &mut |w| visit(Dml::Update { set: set.clone(), from: *from, wher: w, ret: *ret }))?;
            for (i, (c, e)) in set.iter().enumerate() {
                shrink_e(e, &mut |n| {
                    let mut s = set.clone();
                    s[i] = (c.clone(), n);
                    visit(Dml::Update { set: s, from: *from, wher: wher.clone(), ret: *ret })
                })?;
            }
        },
        Dml::Delete { using, wher, ret } => {
            if *ret {
                visit(Dml::Delete { using: *using, wher: wher.clone(), ret: false })?;
            }
            shrink_opt(wher, &mut |w| visit(Dml::Delete { using: *using, wher: w, ret: *ret }))?;
        },
        Dml::InsertSelect { src, items, wher, ret } => {
            if *ret {
                visit(Dml::InsertSelect { src: *src, items: items.clone(), wher: wher.clone(), ret: false })?;
            }
            shrink_opt(wher, &mut |w| visit(Dml::InsertSelect { src: *src, items: items.clone(), wher: w, ret: *ret }))?;
            for (i, e) in items.iter().enumerate().skip(1) {
                shrink_e(e, &mut |n| {
                    let mut s = items.clone();
                    s[i] = n;
                    visit(Dml::InsertSelect { src: *src, items: s, wher: wher.clone(), ret: *ret })
                })?;
            }
        },
        Dml::InsertValues { rows, ret } => {
            if *ret {
                visit(Dml::InsertValues { rows: rows.clone(), ret: false })?;
            }
            if rows.len() > 1 {
                for i in 0..rows.len() {
                    let mut r = rows.clone();
                    r.remove(i);
                    visit(Dml::InsertValues { rows: r, ret: *ret })?;
                }
            }
        },
        Dml::Merge { src, cond, update, delete, insert } => {
            let mk = |cond: Option<E>, update: Option<Vec<(String, E)>>, delete: bool, insert: Option<Vec<E>>| Dml::Merge { src: *src, cond, update, delete, insert };
            if insert.is_some() && (update.is_some() || *delete) {
                visit(mk(cond.clone(), update.clone(), *delete, None))?;
                visit(mk(None, None, false, insert.clone()))?;
            }
            if update.is_some() && *delete {
                visit(mk(cond.clone(), update.clone(), false, insert.clone()))?;
                visit(mk(cond.clone(), None, true, insert.clone()))?;
            }
            shrink_opt(cond, &mut |c| visit(mk(c, update.clone(), *delete && update.is_none(), insert.clone())))?;
            if let Some(u) = update {
                for (i, (c, e)) in u.iter().enumerate() {
                    shrink_e(e, &mut |n| {
                        let mut s = u.clone();
                        s[i] = (c.clone(), n);
                        visit(mk(cond.clone(), Some(s), *delete, insert.clone()))
                    })?;
                }
            }
            if let Some(ins) = insert {
                for (i, e) in ins.iter().enumerate().skip(1) {
                    shrink_e(e, &mut |n| {
                        let mut s = ins.clone();
                        s[i] = n;
                        visit(mk(cond.clone(), update.clone(), *delete, Some(s)))
                    })?;
                }
            }
        },
    }
    ControlFlow::Continue(())
}

fn shrink_dcase(c: &DCase, visit: Visit<DCase>) -> ControlFlow<()> {
    if c.stmts.len() > 1 {
        for i in 0..c.stmts.len() {
            let mut n = c.clone();
            n.stmts.remove(i);
            visit(n)?;
        }
    }
    if c.txn {
        visit(DCase { txn: false, ..c.clone() })?;
    }
    if c.variant != 0 {
        visit(DCase { variant: 0, ..c.clone() })?;
    }
    let as_case = Case { tables: c.tables.clone(), queries: vec![] };
    shrink_tables(&as_case, &mut |tables| visit(DCase { tables, ..c.clone() }))?;
    for (i, d) in c.stmts.iter().enumerate() {
        shrink_dml(d, &mut |n| {
            let mut c2 = c.clone();
            c2.stmts[i] = n;
            visit(c2)
        })?;
    }
    ControlFlow::Continue(())
}

fn cases() -> impl Gen<Value = DCase> {
    (|g: &mut Source| dcase(g)).with_shrink(shrink_dcase)
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

fn kind(d: &Dml) -> &'static str {
    match d {
        Dml::Update { from: Some(_), .. } => "update_from",
        Dml::Update { .. } => "update",
        Dml::Delete { using: Some(_), .. } => "delete_using",
        Dml::Delete { .. } => "delete",
        Dml::InsertSelect { .. } => "insert_select",
        Dml::InsertValues { .. } => "insert_values",
        Dml::Merge { .. } => "merge",
    }
}

fn check(c: &DCase) {
    INIT.call_once(|| {
        let mut s = Script::new();
        s.stmt(&format!("ATTACH {} AS m (TYPE mssql)", sql_lit(&dsn("master"))));
        s.stmt(&format!("SELECT mssql_exec('m', 'IF DB_ID(''{0}'') IS NULL CREATE DATABASE {0}')", db()));
        s.run();
    });
    let as_case = Case { tables: c.tables.clone(), queries: vec![] };
    let mut s = Script::new();
    s.stmt(&format!("ATTACH {} AS a (TYPE mssql)", sql_lit(&dsn(&db()))));
    s.stmt("ATTACH ':memory:' AS l");
    s.stmt("SELECT mssql_exec('a', 'DROP TABLE IF EXISTS dbo.t0, dbo.t1, dbo.t2')");
    s.stmt(&format!("SELECT mssql_exec('a', {})", sql_lit(&as_case.setup_tsql())));
    for i in 0..c.tables.len() {
        s.stmt(&format!("CREATE TABLE l.t{i} AS SELECT * FROM a.dbo.t{i}"));
    }
    match c.variant {
        0 => {},
        1 => { s.stmt("SET mssql_dml_pushdown = false"); },
        2 => { s.stmt("SET mssql_dml_pushdown = false"); s.stmt("SET mssql_dml_stage_threshold = 0"); },
        _ => {
            s.stmt("SET mssql_dml_pushdown = false");
            s.stmt("SET mssql_insert_use_bcp = false");
            s.stmt("SET mssql_dml_batch_size = 2");
            s.stmt("SET mssql_insert_batch_size = 2");
        },
    }
    // per side, per statement: (statement section, snapshot section)
    let mut idx: Vec<Vec<(usize, usize)>> = Vec::new();
    let mut txn_errs = None;
    for p in ["l.", "a.dbo."] {
        let mut v = Vec::new();
        let txn = c.txn && p != "l.";
        if txn {
            s.stmt("BEGIN");
        }
        for d in &c.stmts {
            let (st, snap) = if txn {
                (s.section_quiet(&d.sql(p)), s.section_quiet(&format!("SELECT * FROM {p}t0")))
            } else {
                (s.section(&d.sql(p)), s.section(&format!("SELECT * FROM {p}t0")))
            };
            v.push((st, snap));
        }
        if txn {
            s.stmt("COMMIT");
            // every error of the transaction lands here
            txn_errs = Some(s.section("SELECT 1"));
            let snap = s.section(&format!("SELECT * FROM {p}t0"));
            let last = v.len() - 1;
            // after the commit the table must still hold what the last statement left
            v.push((v[last].0, snap));
        }
        idx.push(v);
    }
    let out = s.run();
    if let Some(sig) = crash(&out) {
        fail(sig, format!("{:#?}\n{}", out.pre.errs, out.script));
    }
    if !out.pre.errs.is_empty() {
        fail(format!("setup:{}", words(&out.pre.errs[0].1, 6)), format!("{:?}\n{}", out.pre.errs, out.script));
    }
    if let Some(t) = txn_errs {
        if let Some((_, msg)) = out.sections[t].errs.first() {
            let l_failed = idx[0].iter().any(|(st, _)| !out.sections[*st].errs.is_empty());
            if !l_failed && (msg.contains("SQL Server error") || msg.contains("] ")) {
                fail(format!("dml:txn:server_error:{}", words(msg, 8)), format!("{msg}\n{}", out.script));
            }
            ephyra::classify(true, "txn error");
            return;
        }
    }
    let sorted = |i: usize| {
        let mut r = out.sections[i].rows.clone();
        r.sort();
        r
    };
    let mut prev_a: Option<Vec<String>> = None;
    for step in 0..idx[1].len() {
        let (ls, lsnap) = idx[0][step.min(idx[0].len() - 1)];
        let (as_, asnap) = idx[1][step];
        let d = &c.stmts[step.min(c.stmts.len() - 1)];
        let k = kind(d);
        let v = c.variant;
        let detail = |what: &str| format!("{what}\nstatement: {}\n{}", d.sql("a.dbo."), out.script);
        let (lerr, aerr) = (out.sections[ls].errs.first(), out.sections[as_].errs.first());
        match (lerr, aerr) {
            (None, Some((ty, msg))) => {
                // unchanged table required either way
                if let Some(p) = &prev_a {
                    if !c.txn && *p != sorted(asnap) {
                        fail(format!("dml:{k}:v{v}:failed_statement_changed_rows"), detail(msg));
                    }
                }
                if msg.contains("SQL Server error") || msg.contains("] ") || ty == "IO" {
                    fail(format!("dml:{k}:v{v}:server_error:{}", words(msg, 8)), detail(msg));
                }
                let dir = std::path::Path::new(".ephyra/refusals");
                let _ = std::fs::create_dir_all(dir);
                let _ = std::fs::write(dir.join(format!("dml_{k}_{}.txt", words(msg, 8))), detail(msg));
                ephyra::classify(true, "refused");
                return;
            },
            // DuckDB evaluates a SET value for rows the condition excludes; the server does not
            (Some((_, msg)), None) if msg.contains("Division by zero") || msg.contains("Overflow in") => {
                ephyra::classify(true, "lazy division");
                return;
            },
            (Some((_, msg)), None) => fail(format!("dml:{k}:v{v}:missing_error:{}", words(msg, 6)), detail(msg)),
            (Some(_), Some(_)) => ephyra::classify(true, "both_err"),
            (None, None) => {
                if sorted(ls) != sorted(as_) {
                    fail(
                        format!("dml:{k}:v{v}:output"),
                        detail(&format!("want {:?}\nhave {:?}", sorted(ls), sorted(as_))),
                    );
                }
            },
        }
        if let Some((_, msg)) = out.sections[asnap].errs.first() {
            if c.txn {
                // a failed statement may have ended the transaction; nothing more to compare
                ephyra::classify(true, "txn aborted");
                return;
            }
            fail(format!("dml:{k}:v{v}:read_after:{}", words(msg, 6)), detail(msg));
        }
        let (want, have) = (sorted(lsnap), sorted(asnap));
        if want != have {
            let t = if c.txn { ":txn" } else { "" };
            fail(format!("dml:{k}:v{v}{t}:rows"), detail(&format!("want {want:#?}\nhave {have:#?}")));
        }
        prev_a = Some(have);
        ephyra::classify(true, "compared");
    }
}

#[ephyra::test(gen = cases(), tests = 200, failures = 25)]
fn dmldiff(c: DCase) {
    check(&c);
}
