//! Model, generator, renderer, shrinker and driver for the mssql-extension
//! property tests. See NOTES.md for the properties and their sources.
use ephyra::Source;
use std::ops::ControlFlow;

pub mod driver;

// ---------------------------------------------------------------- model

#[derive(Clone, Debug, PartialEq)]
pub enum Ty {
    TinyInt,
    SmallInt,
    Int,
    BigInt,
    Bit,
    Dec(u8, u8),
    Float,
    Date,
    DateTime2,
    DateTime,
    /// true: Latin1_General_100_BIN2_UTF8; false: the database default (CI_AS, CP1252)
    VarChar(bool),
    NVarChar,
    Uuid,
    Real,
    Money,
    SmallMoney,
    /// datetime2(7): read as TIMESTAMP_NS
    DateTime27,
    Time7,
    Dto7,
    SmallDateTime,
    Char,
    NChar,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Class {
    Int,
    Dec,
    Float,
    Bool,
    Str,
    Date,
    Ts,
    Uuid,
    Time,
    Tz,
}

impl Ty {
    pub fn class(&self) -> Class {
        match self {
            Ty::TinyInt | Ty::SmallInt | Ty::Int | Ty::BigInt => Class::Int,
            Ty::Bit => Class::Bool,
            Ty::Dec(..) | Ty::Money | Ty::SmallMoney => Class::Dec,
            Ty::Float | Ty::Real => Class::Float,
            Ty::Time7 => Class::Time,
            Ty::Dto7 => Class::Tz,
            Ty::Date => Class::Date,
            Ty::DateTime2 | Ty::DateTime | Ty::DateTime27 | Ty::SmallDateTime => Class::Ts,
            Ty::VarChar(_) | Ty::NVarChar | Ty::Char | Ty::NChar => Class::Str,
            Ty::Uuid => Class::Uuid,
        }
    }
    pub fn ddl(&self) -> String {
        match self {
            Ty::TinyInt => "tinyint".into(),
            Ty::SmallInt => "smallint".into(),
            Ty::Int => "int".into(),
            Ty::BigInt => "bigint".into(),
            Ty::Bit => "bit".into(),
            Ty::Dec(p, s) => format!("decimal({p},{s})"),
            Ty::Float => "float".into(),
            Ty::Date => "date".into(),
            Ty::DateTime2 => "datetime2(6)".into(),
            Ty::DateTime => "datetime".into(),
            Ty::VarChar(true) => "varchar(20) COLLATE Latin1_General_100_BIN2_UTF8".into(),
            // binary collations only: a pushed comparison follows the collation (README, #272)
            Ty::VarChar(false) => "varchar(20) COLLATE Latin1_General_100_BIN2".into(),
            Ty::NVarChar => "nvarchar(20) COLLATE Latin1_General_100_BIN2".into(),
            Ty::Uuid => "uniqueidentifier".into(),
            Ty::Real => "real".into(),
            Ty::Money => "money".into(),
            Ty::SmallMoney => "smallmoney".into(),
            Ty::DateTime27 => "datetime2(7)".into(),
            Ty::Time7 => "time(7)".into(),
            Ty::Dto7 => "datetimeoffset(7)".into(),
            Ty::SmallDateTime => "smalldatetime".into(),
            Ty::Char => "char(5) COLLATE Latin1_General_100_BIN2_UTF8".into(),
            Ty::NChar => "nchar(5) COLLATE Latin1_General_100_BIN2".into(),
        }
    }
    /// A raw value as a T-SQL literal.
    pub fn tsql(&self, v: &str) -> String {
        match self.class() {
            Class::Int | Class::Dec | Class::Float | Class::Bool => v.to_string(),
            _ if *self == Ty::NVarChar => format!("N'{}'", v.replace('\'', "''")),
            _ => format!("'{}'", v.replace('\'', "''")),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Col {
    pub name: String,
    pub ty: Ty,
}

/// Column 0 is always `k int NOT NULL`, unique (the row's number).
#[derive(Clone, Debug, PartialEq)]
pub struct Table {
    pub pk: bool,
    pub cols: Vec<Col>,
    pub rows: Vec<Vec<Option<String>>>,
}

#[derive(Clone, Debug, PartialEq)]
pub enum E {
    Col(String),
    Lit(String),
    Un(&'static str, Box<E>),
    Bin(&'static str, Box<E>, Box<E>),
    Fn(&'static str, Vec<E>),
    Case(Box<E>, Box<E>, Box<E>),
    In(Box<E>, Vec<E>, bool),
    Between(Box<E>, Box<E>, Box<E>),
    IsNull(Box<E>, bool),
    /// name, distinct, argument (None = `*`)
    Agg(&'static str, bool, Option<Box<E>>),
    /// function OVER (PARTITION BY .. ORDER BY ..): name, args, partition, order
    Win(&'static str, Vec<E>, Vec<E>, Vec<E>),
    /// [NOT] EXISTS / IN subquery: kind, lhs, subquery
    Sub(&'static str, Option<Box<E>>, Box<Q>),
}

#[derive(Clone, Debug, PartialEq)]
pub struct From {
    pub table: usize,
    pub join: &'static str,
    pub on: Option<E>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Q {
    pub distinct: bool,
    pub items: Vec<E>,
    /// first alias number of this query's FROM items (subqueries use their own range)
    pub base: usize,
    pub from: Vec<From>,
    pub wher: Option<E>,
    /// Some(n): GROUP BY the first n items (n may be 0: one group)
    pub group: Option<usize>,
    pub having: Option<E>,
    /// one (desc, nulls_first) per item: ORDER BY every output column
    pub order: Option<Vec<(bool, bool)>>,
    pub limit: Option<(u32, u32)>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Case {
    pub tables: Vec<Table>,
    pub queries: Vec<Q>,
}

// ---------------------------------------------------------------- render

impl E {
    pub fn sql(&self, p: &str) -> String {
        match self {
            E::Col(c) => c.clone(),
            E::Lit(l) => l.clone(),
            E::Un(op, a) => format!("({op} {})", a.sql(p)),
            E::Bin(op, a, b) => format!("({} {op} {})", a.sql(p), b.sql(p)),
            E::Fn("to_int", args) => format!("CAST({} AS INTEGER)", args[0].sql(p)),
            E::Fn(f, args) => format!("{f}({})", args.iter().map(|a| a.sql(p)).collect::<Vec<_>>().join(", ")),
            E::Case(c, a, b) => format!("CASE WHEN {} THEN {} ELSE {} END", c.sql(p), a.sql(p), b.sql(p)),
            E::In(a, l, neg) => format!(
                "({} {}IN ({}))",
                a.sql(p),
                if *neg { "NOT " } else { "" },
                l.iter().map(|a| a.sql(p)).collect::<Vec<_>>().join(", ")
            ),
            E::Between(a, lo, hi) => format!("({} BETWEEN {} AND {})", a.sql(p), lo.sql(p), hi.sql(p)),
            E::IsNull(a, neg) => format!("({} IS {}NULL)", a.sql(p), if *neg { "NOT " } else { "" }),
            E::Agg(f, d, a) => match a {
                None => format!("{f}(*)"),
                Some(a) => format!("{f}({}{})", if *d { "DISTINCT " } else { "" }, a.sql(p)),
            },
            E::Win(f, args, part, ord) => {
                let l = |v: &Vec<E>| v.iter().map(|a| a.sql(p)).collect::<Vec<_>>().join(", ");
                let mut over = String::new();
                if !part.is_empty() {
                    over += &format!("PARTITION BY {}", l(part));
                }
                if !ord.is_empty() {
                    over += &format!(" ORDER BY {}", l(ord));
                }
                format!("{f}({}) OVER ({})", l(args), over.trim())
            },
            E::Sub(kind, lhs, q) => match lhs {
                Some(l) => format!("({} {kind} ({}))", l.sql(p), q.sql(p)),
                None => format!("({kind} ({}))", q.sql(p)),
            },
        }
    }
}

impl Q {
    pub fn sql(&self, p: &str) -> String {
        let mut s = String::from("SELECT ");
        if self.distinct {
            s += "DISTINCT ";
        }
        let items: Vec<String> = self.items.iter().enumerate().map(|(i, e)| format!("{} AS c{i}", e.sql(p))).collect();
        s += &items.join(", ");
        s += " FROM ";
        for (i, f) in self.from.iter().enumerate() {
            let alias = self.base + i;
            if i > 0 {
                s += &format!(" {} ", f.join);
            }
            s += &format!("{p}t{} AS x{alias}", f.table);
            if let (true, Some(on)) = (i > 0 && f.join != "CROSS JOIN", &f.on) {
                s += &format!(" ON {}", on.sql(p));
            }
        }
        if let Some(w) = &self.wher {
            s += &format!(" WHERE {}", w.sql(p));
        }
        if let Some(n) = self.group {
            if n > 0 {
                s += &format!(" GROUP BY {}", (1..=n).map(|i| i.to_string()).collect::<Vec<_>>().join(", "));
            }
            if let Some(h) = &self.having {
                s += &format!(" HAVING {}", h.sql(p));
            }
        }
        if let Some(o) = &self.order {
            let keys: Vec<String> = (0..self.items.len())
                .map(|i| {
                    let (desc, nf) = o.get(i).copied().unwrap_or((false, false));
                    format!("{} {} NULLS {}", i + 1, if desc { "DESC" } else { "ASC" }, if nf { "FIRST" } else { "LAST" })
                })
                .collect();
            s += &format!(" ORDER BY {}", keys.join(", "));
            if let Some((l, off)) = self.limit {
                s += &format!(" LIMIT {l}");
                if off > 0 {
                    s += &format!(" OFFSET {off}");
                }
            }
        }
        s
    }
}

impl Case {
    /// The T-SQL batch that creates and fills the tables.
    pub fn setup_tsql(&self) -> String {
        let mut s = String::new();
        for (i, t) in self.tables.iter().enumerate() {
            let cols: Vec<String> = t
                .cols
                .iter()
                .enumerate()
                .map(|(j, c)| {
                    if j == 0 {
                        format!("k int NOT NULL{}", if t.pk { " PRIMARY KEY" } else { "" })
                    } else {
                        format!("{} {}", c.name, c.ty.ddl())
                    }
                })
                .collect();
            s += &format!(" CREATE TABLE dbo.t{i} ({});", cols.join(", "));
            for chunk in t.rows.chunks(500) {
                let rows: Vec<String> = chunk
                    .iter()
                    .map(|r| {
                        let vals: Vec<String> = r
                            .iter()
                            .zip(&t.cols)
                            .map(|(v, c)| v.as_ref().map_or("NULL".to_string(), |v| c.ty.tsql(v)))
                            .collect();
                        format!("({})", vals.join(", "))
                    })
                    .collect();
                s += &format!(" INSERT INTO dbo.t{i} VALUES {};", rows.join(", "));
            }
        }
        s
    }
}

// ---------------------------------------------------------------- generator

// No trailing spaces: a pushed comparison pads them (README, "A note on string comparisons", #272).
const STRS: &[&str] = &["", "a", "b", "ab", "A", "ba", "é", "ß", "a%", "B"];
const DATES: &[&str] = &["2020-01-01", "1999-12-31", "2024-02-29", "0001-01-01", "9999-12-31", "2021-03-04"];
const TS6: &[&str] = &[
    "2020-01-01 00:00:00",
    "2021-03-04 05:06:07.123456",
    "1900-01-01 00:00:00",
    "9999-12-31 23:59:59.999999",
    "0001-01-01 00:00:00",
    "2021-03-04 05:06:07.997",
];
const TS3: &[&str] =
    &["2020-01-01 00:00:00", "2021-03-04 05:06:07.997", "1753-01-01 00:00:00", "2021-03-04 05:06:07.003", "2021-03-04 05:06:07.123"];
const TS7: &[&str] = &[
    "2021-03-04 05:06:07.1234567",
    "2020-01-01 00:00:00",
    "1900-01-01 00:00:00",
    "2262-04-11 23:47:16.8547758",
    "2021-03-04 05:06:07.1234569",
    "1677-09-22 00:00:00",
    "2021-03-04 05:06:07.123456",
];
const UUIDS: &[&str] = &[
    "00000000-0000-0000-0000-000000000000",
    "6f9619ff-8b86-d011-b42d-00c04fc964ff",
    "ffffffff-ffff-ffff-ffff-ffffffffffff",
    "01020304-0506-0708-090a-0b0c0d0e0f10",
];
const FLOATS: &[&str] = &["0", "0.25", "-1.5", "2", "1e10", "0.1", "7"];

fn dec_value(g: &mut Source, p: u8, s: u8) -> String {
    let scale = |v: &str| -> String {
        // v has two decimals; fit to scale s
        let (int, frac) = v.split_once('.').unwrap();
        match s {
            0 => int.to_string(),
            1 => format!("{int}.{}", &frac[..1]),
            _ => format!("{int}.{frac}{}", "0".repeat(s as usize - 2)),
        }
    };
    if g.bool_with(0.08) {
        let int = "9".repeat((p - s) as usize);
        let sign = if g.bool() { "-" } else { "" };
        return if s == 0 { format!("{sign}{int}") } else { format!("{sign}{int}.{}", "9".repeat(s as usize)) };
    }
    scale(g.pick(&["0.00", "1.50", "-2.25", "7.00", "0.50", "-0.50", "3.00", "2.00"]))
}

fn value(g: &mut Source, ty: &Ty) -> Option<String> {
    if g.bool_with(0.15) {
        return None;
    }
    let edge = g.bool_with(0.08);
    let small = |g: &mut Source| g.pick(&["-1", "0", "1", "2", "3", "7"]).to_string();
    Some(match ty {
        Ty::TinyInt => if edge { "255".into() } else { g.pick(&["0", "1", "2", "3", "7"]).to_string() },
        Ty::SmallInt => if edge { g.pick(&["32767", "-32768"]).to_string() } else { small(g) },
        Ty::Int => if edge { g.pick(&["2147483647", "-2147483648"]).to_string() } else { small(g) },
        Ty::BigInt => {
            if edge { g.pick(&["9223372036854775807", "-9223372036854775808", "4294967296"]).to_string() } else { small(g) }
        },
        Ty::Bit => g.pick(&["0", "1"]).to_string(),
        Ty::Dec(p, s) => dec_value(g, *p, *s),
        Ty::Float => g.pick(FLOATS).to_string(),
        Ty::Date => g.pick(DATES).to_string(),
        Ty::DateTime2 => g.pick(TS6).to_string(),
        Ty::DateTime => g.pick(TS3).to_string(),
        Ty::VarChar(_) => g.pick(STRS).to_string(),
        Ty::NVarChar => if edge { "日本".into() } else { g.pick(STRS).to_string() },
        Ty::Uuid => g.pick(UUIDS).to_string(),
        Ty::Real => g.pick(FLOATS).to_string(),
        Ty::Money => g.pick(&["0.00", "1.50", "-2.25", "7.00", "922337203685477.5807", "0.0001", "3.00"]).to_string(),
        Ty::SmallMoney => g.pick(&["0.00", "1.50", "-2.25", "7.00", "214748.3647", "0.0001", "3.00"]).to_string(),
        Ty::DateTime27 => g.pick(TS7).to_string(),
        Ty::Time7 => g.pick(&["00:00:00", "12:34:56", "23:59:59.9999999", "12:34:56.1234567", "12:34:56.123456", "23:59:59.999999"]).to_string(),
        Ty::Dto7 => g.pick(&[
            "2021-03-04 05:06:07.1234567 +02:00",
            "2020-01-01 00:00:00 +00:00",
            "2020-01-01 00:00:00 -08:00",
            "2021-03-04 03:06:07.123456 +00:00",
            "2020-01-01 08:00:00 +00:00",
        ]).to_string(),
        Ty::SmallDateTime => g.pick(&["2020-01-01 00:00:00", "2021-03-04 05:06:00", "1900-01-01 00:00:00", "2079-06-06 23:59:00"]).to_string(),
        Ty::Char | Ty::NChar => g.pick(&["", "a", "ab", "A", "abcde", "b", "é"]).to_string(),
    })
}

fn ty(g: &mut Source) -> Ty {
    match g.int(0u32..=28) {
        20 => Ty::Real,
        21 => Ty::Money,
        22 => Ty::SmallMoney,
        23 => Ty::DateTime27,
        24 => Ty::Time7,
        25 => Ty::Dto7,
        26 => Ty::SmallDateTime,
        27 => Ty::Char,
        28 => Ty::NChar,
        0 => Ty::TinyInt,
        1 => Ty::SmallInt,
        2..=4 => Ty::Int,
        5 => Ty::BigInt,
        6 | 7 => Ty::Bit,
        8 | 9 => {
            let (p, s) = *g.pick(&[(10u8, 2u8), (18, 4), (38, 10), (5, 0), (9, 1)]);
            Ty::Dec(p, s)
        },
        10 => Ty::Float,
        11 | 12 => Ty::Date,
        13 => Ty::DateTime2,
        14 => Ty::DateTime,
        15 | 16 => Ty::VarChar(true),
        17 => Ty::VarChar(false),
        18 => Ty::NVarChar,
        _ => Ty::Uuid,
    }
}

pub fn table(g: &mut Source) -> Table {
    let ncols = g.int(1usize..=5);
    let mut cols = vec![Col { name: "k".into(), ty: Ty::Int }];
    for i in 0..ncols {
        cols.push(Col { name: format!("c{i}"), ty: ty(g) });
    }
    let nrows = if g.bool_with(0.1) { 0 } else { g.int(1usize..=12) };
    let rows = (0..nrows)
        .map(|r| {
            let mut row = vec![Some(r.to_string())];
            for c in &cols[1..] {
                row.push(value(g, &c.ty));
            }
            row
        })
        .collect();
    Table { pk: g.bool_with(0.6), cols, rows }
}

/// What the expression generator sees: (alias, table).
pub struct Scope<'a> {
    pub cols: Vec<(String, &'a Ty)>,
    pub tables: &'a [Table],
    /// next free alias number for subqueries
    pub next_alias: usize,
    pub allow_sub: bool,
    /// only forms the remote-pushdown writer takes (measured with EXPLAIN, NOTES.md)
    pub pushable: bool,
}

pub fn lit(g: &mut Source, c: Class) -> E {
    if g.bool_with(0.05) {
        return E::Lit("NULL".into());
    }
    E::Lit(match c {
        Class::Int => {
            if g.bool_with(0.05) { "2147483647".into() } else { g.pick(&["-1", "0", "1", "2", "3", "7"]).to_string() }
        },
        Class::Dec => g.pick(&["0.00", "1.50", "-2.25", "7.00", "0.5", "3"]).to_string(),
        Class::Float => format!("{}::DOUBLE", g.pick(FLOATS)),
        Class::Bool => g.pick(&["true", "false"]).to_string(),
        Class::Str => format!("'{}'", g.pick(STRS)),
        Class::Date => format!("DATE '{}'", g.pick(DATES)),
        Class::Ts => format!("TIMESTAMP '{}'", match g.int(0u32..=2) { 0 => g.pick(TS6), 1 => g.pick(TS3), _ => g.pick(TS7) }),
        Class::Uuid => format!("'{}'::UUID", g.pick(UUIDS)),
        Class::Time => format!("TIME '{}'", g.pick(&["00:00:00", "12:34:56", "23:59:59.999999", "12:34:56.123456", "12:34:56.123457"])),
        Class::Tz => format!(
            "TIMESTAMPTZ '{}'",
            g.pick(&["2021-03-04 03:06:07.123456+00", "2020-01-01 00:00:00+00", "2020-01-01 08:00:00+00", "2021-03-04 03:06:07.123457+00"])
        ),
    })
}

fn col(g: &mut Source, sc: &Scope, c: Class) -> Option<E> {
    let m: Vec<&String> = sc.cols.iter().filter(|(_, t)| t.class() == c).map(|(n, _)| n).collect();
    if m.is_empty() { None } else { Some(E::Col(m[g.index(m.len())].clone())) }
}

fn bx(e: E) -> Box<E> {
    Box::new(e)
}

pub fn expr(g: &mut Source, sc: &mut Scope, c: Class, depth: usize) -> E {
    if depth == 0 || g.bool_with(0.45) {
        if g.bool_with(0.75) {
            if let Some(e) = col(g, sc, c) {
                return e;
            }
        }
        return lit(g, c);
    }
    let d = depth - 1;
    if sc.pushable {
        return pushable_expr(g, sc, c, d);
    }
    // generic forms
    match g.int(0u32..=9) {
        0 => return E::Case(bx(expr(g, sc, Class::Bool, d)), bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
        1 => return E::Fn("coalesce", vec![expr(g, sc, c, d), expr(g, sc, c, d)]),
        _ => {},
    }
    match c {
        Class::Int => match g.int(0u32..=7) {
            0 | 1 => E::Bin(g.pick(&["+", "-", "*"]), bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
            2 => E::Un("-", bx(expr(g, sc, c, d))),
            3 => E::Fn("abs", vec![expr(g, sc, c, d)]),
            4 => E::Fn(g.pick(&["year", "month", "day"]), vec![expr(g, sc, Class::Date, d)]),
            5 => E::Fn("length", vec![expr(g, sc, Class::Str, d)]),
            6 => E::Bin(g.pick(&["%", "//"]), bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
            _ => E::Fn("to_int", vec![expr(g, sc, Class::Bool, d)]),
        },
        Class::Dec => match g.int(0u32..=2) {
            0 => E::Bin(g.pick(&["+", "-", "*"]), bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
            1 => E::Un("-", bx(expr(g, sc, c, d))),
            _ => E::Bin("+", bx(expr(g, sc, c, d)), bx(expr(g, sc, Class::Int, d))),
        },
        Class::Float => E::Fn("coalesce", vec![expr(g, sc, c, d), expr(g, sc, c, d)]),
        Class::Str => match g.int(0u32..=3) {
            0 => E::Fn(g.pick(&["upper", "lower", "trim"]), vec![expr(g, sc, c, d)]),
            1 => E::Bin("||", bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
            2 => E::Fn("substr", vec![expr(g, sc, c, d), lit(g, Class::Int), lit(g, Class::Int)]),
            _ => E::Fn("coalesce", vec![expr(g, sc, c, d), lit(g, c)]),
        },
        Class::Date | Class::Ts | Class::Uuid | Class::Time | Class::Tz => E::Fn("coalesce", vec![expr(g, sc, c, d), lit(g, c)]),
        Class::Bool => match g.int(0u32..=11) {
            0..=3 => {
                let cc = *g.pick(&[Class::Int, Class::Int, Class::Dec, Class::Float, Class::Str, Class::Str, Class::Date, Class::Ts, Class::Ts, Class::Uuid, Class::Time, Class::Tz]);
                let rc = if cc == Class::Int && g.bool_with(0.2) { Class::Dec } else { cc };
                E::Bin(g.pick(&["=", "<>", "<", "<=", ">", ">="]), bx(expr(g, sc, cc, d)), bx(expr(g, sc, rc, d)))
            },
            4 | 5 => E::Bin(g.pick(&["AND", "OR"]), bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
            6 => E::Un("NOT", bx(expr(g, sc, c, d))),
            7 => {
                let cc = *g.pick(&[Class::Int, Class::Dec, Class::Str, Class::Date, Class::Ts, Class::Bool, Class::Float]);
                E::IsNull(bx(expr(g, sc, cc, d)), g.bool())
            },
            8 => {
                let cc = *g.pick(&[Class::Int, Class::Dec, Class::Str, Class::Date, Class::Ts, Class::Time, Class::Tz]);
                let n = g.int(1usize..=3);
                E::In(bx(expr(g, sc, cc, d)), (0..n).map(|_| lit(g, cc)).collect(), g.bool_with(0.3))
            },
            9 => {
                let cc = *g.pick(&[Class::Int, Class::Dec, Class::Str, Class::Date, Class::Ts, Class::Time, Class::Tz]);
                E::Between(bx(expr(g, sc, cc, d)), bx(lit(g, cc)), bx(lit(g, cc)))
            },
            10 => E::Bin(
                g.pick(&["LIKE", "NOT LIKE"]),
                bx(expr(g, sc, Class::Str, d)),
                bx(E::Lit(format!("'{}'", g.pick(&["a%", "%b", "_", "%", "a_", "%a%", "A%", "a\\%"])))),
            ),
            _ if sc.allow_sub => subquery(g, sc),
            _ => E::IsNull(bx(expr(g, sc, Class::Int, d)), true),
        },
    }
}

/// Expressions of the forms the writer renders: arithmetic, CASE, COALESCE, comparisons of
/// columns with constants of their own kind, IN / BETWEEN / LIKE / IS NULL, AND / OR / NOT.
fn pushable_expr(g: &mut Source, sc: &mut Scope, c: Class, d: usize) -> E {
    let leaf = |g: &mut Source, sc: &Scope, c: Class| col(g, sc, c).unwrap_or_else(|| lit(g, c));
    match c {
        Class::Int | Class::Dec => match g.int(0u32..=5) {
            0 | 1 => E::Bin(g.pick(&["+", "-", "*"]), bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
            2 => E::Un("-", bx(expr(g, sc, c, d))),
            3 => E::Case(bx(expr(g, sc, Class::Bool, d)), bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
            4 => E::Fn("coalesce", vec![expr(g, sc, c, d), expr(g, sc, c, d)]),
            _ if c == Class::Int => E::Bin("%", bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
            _ => leaf(g, sc, c),
        },
        Class::Bool => match g.int(0u32..=11) {
            0..=4 => {
                let cc = *g.pick(&[Class::Int, Class::Int, Class::Dec, Class::Str, Class::Date]);
                let l = if cc == Class::Str || cc == Class::Date { leaf(g, sc, cc) } else { expr(g, sc, cc, d) };
                let r = if g.bool_with(0.6) { lit(g, cc) } else { leaf(g, sc, cc) };
                E::Bin(g.pick(&["=", "<>", "<", "<=", ">", ">="]), bx(l), bx(r))
            },
            5 | 6 => E::Bin(g.pick(&["AND", "OR"]), bx(expr(g, sc, c, d)), bx(expr(g, sc, c, d))),
            7 => E::Un("NOT", bx(expr(g, sc, c, d))),
            8 => {
                let cc = any_class(g);
                E::IsNull(bx(leaf(g, sc, cc)), g.bool())
            },
            9 => {
                let cc = *g.pick(&[Class::Int, Class::Dec, Class::Str, Class::Date]);
                let n = g.int(1usize..=3);
                if g.bool() {
                    E::In(bx(leaf(g, sc, cc)), (0..n).map(|_| lit(g, cc)).collect(), g.bool_with(0.3))
                } else {
                    E::Between(bx(leaf(g, sc, cc)), bx(lit(g, cc)), bx(lit(g, cc)))
                }
            },
            10 => E::Bin(
                g.pick(&["LIKE", "NOT LIKE"]),
                bx(leaf(g, sc, Class::Str)),
                bx(E::Lit(format!("'{}'", g.pick(&["a%", "%b", "_", "%", "a_", "%a%", "A%"])))),
            ),
            _ if sc.allow_sub => subquery(g, sc),
            _ => leaf(g, sc, Class::Bool),
        },
        _ => leaf(g, sc, c),
    }
}

pub fn scope_of<'a>(tables: &'a [Table], from: &[From], base: usize) -> Vec<(String, &'a Ty)> {
    let mut v = Vec::new();
    for (i, f) in from.iter().enumerate() {
        for c in &tables[f.table].cols {
            v.push((format!("x{}.{}", base + i, c.name), &c.ty));
        }
    }
    v
}

/// A correlated [NOT] EXISTS or an [NOT] IN over one table.
fn subquery(g: &mut Source, sc: &mut Scope) -> E {
    let base = sc.next_alias;
    sc.next_alias += 1;
    let from = vec![From { table: g.index(sc.tables.len()), join: "", on: None }];
    let mut inner = Scope { cols: scope_of(sc.tables, &from, base), tables: sc.tables, next_alias: sc.next_alias, allow_sub: false, pushable: sc.pushable };
    let own = inner.cols.clone();
    inner.cols.extend(sc.cols.iter().cloned());
    let kind = *g.pick(&["EXISTS", "NOT EXISTS", "IN", "NOT IN"]);
    let wher = if g.bool_with(0.8) { Some(expr(g, &mut inner, Class::Bool, 2)) } else { None };
    let (lhs, item) = if kind.ends_with("IN") {
        let cc = *g.pick(&[Class::Int, Class::Int, Class::Dec, Class::Date]);
        let mut own_sc = Scope { cols: own, tables: sc.tables, next_alias: 0, allow_sub: false, pushable: sc.pushable };
        (Some(bx(expr(g, sc, cc, 1))), expr(g, &mut own_sc, cc, 1))
    } else {
        (None, E::Lit("1".into()))
    };
    let q = Q { distinct: false, items: vec![item], base, from, wher, group: None, having: None, order: None, limit: None };
    E::Sub(kind, lhs, Box::new(q))
}

fn agg(g: &mut Source, sc: &mut Scope) -> E {
    match g.int(0u32..=6) {
        0 => E::Agg("count", false, None),
        1 => {
            let cc = *g.pick(&[Class::Int, Class::Str, Class::Date, Class::Dec, Class::Bool]);
            E::Agg("count", g.bool_with(0.4), Some(bx(expr(g, sc, cc, 1))))
        },
        2 | 3 => {
            let cc = *g.pick(&[Class::Int, Class::Int, Class::Dec]);
            E::Agg(g.pick(&["sum", "sum", "avg"]), g.bool_with(0.15), Some(bx(expr(g, sc, cc, 1))))
        },
        _ => {
            let cc = *g.pick(&[Class::Int, Class::Dec, Class::Float, Class::Str, Class::Date, Class::Ts, Class::Uuid, Class::Time, Class::Tz]);
            E::Agg(g.pick(&["min", "max"]), false, Some(bx(expr(g, sc, cc, 1))))
        },
    }
}

fn any_class(g: &mut Source) -> Class {
    *g.pick(&[Class::Int, Class::Int, Class::Dec, Class::Float, Class::Bool, Class::Str, Class::Date, Class::Ts, Class::Uuid, Class::Time, Class::Tz])
}

/// A window function whose order is total: it ends with every `k` in scope.
fn window(g: &mut Source, sc: &mut Scope, ks: &[String]) -> E {
    let part = if g.bool_with(0.6) { let c = any_class(g); vec![expr(g, sc, c, 0)] } else { vec![] };
    let mut ord = Vec::new();
    if g.bool_with(0.4) {
        let cc = *g.pick(&[Class::Int, Class::Dec, Class::Date]);
        ord.push(expr(g, sc, cc, 0));
    }
    ord.extend(ks.iter().map(|k| E::Col(k.clone())));
    match g.int(0u32..=5) {
        0 | 1 => E::Win(g.pick(&["row_number", "rank", "dense_rank"]), vec![], part, ord),
        2 => E::Win("sum", vec![expr(g, sc, Class::Int, 0)], part, ord),
        3 => E::Win("count", vec![expr(g, sc, Class::Int, 0)], part, ord),
        4 => {
            let cc = *g.pick(&[Class::Int, Class::Dec, Class::Date, Class::Str]);
            E::Win(g.pick(&["lag", "lead", "first_value", "last_value"]), vec![expr(g, sc, cc, 0)], part, ord)
        },
        _ => {
            let cc = *g.pick(&[Class::Int, Class::Dec, Class::Date]);
            E::Win(g.pick(&["min", "max"]), vec![expr(g, sc, cc, 0)], part, vec![])
        },
    }
}

pub fn query(g: &mut Source, tables: &[Table]) -> Q {
    let pushable = g.bool_with(0.6);
    let nfrom = *g.pick(&[1usize, 1, 1, 1, 2, 2, 2, 3]);
    let mut from: Vec<From> = Vec::new();
    for i in 0..nfrom {
        let table = g.index(tables.len());
        if i == 0 {
            from.push(From { table, join: "", on: None });
            continue;
        }
        let join = *g.pick(&["JOIN", "JOIN", "LEFT JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN", "CROSS JOIN", "SEMI JOIN", "ANTI JOIN"]);
        from.push(From { table, join, on: None });
        // the right side of an earlier SEMI / ANTI join is not visible
        let hidden: Vec<String> =
            from[..i].iter().enumerate().filter(|(_, f)| f.join == "SEMI JOIN" || f.join == "ANTI JOIN").map(|(j, _)| format!("x{j}.")).collect();
        let cols = scope_of(tables, &from, 0).into_iter().filter(|(n, _)| !hidden.iter().any(|h| n.starts_with(h))).collect();
        let mut sc = Scope { cols, tables, next_alias: nfrom, allow_sub: false, pushable };
        let on = if g.bool_with(0.7) {
            // an equality between an integer column of the new table and one of an earlier table
            let new: Vec<String> =
                sc.cols.iter().filter(|(n, t)| n.starts_with(&format!("x{i}.")) && t.class() == Class::Int).map(|(n, _)| n.clone()).collect();
            let old: Vec<String> =
                sc.cols.iter().filter(|(n, t)| !n.starts_with(&format!("x{i}.")) && t.class() == Class::Int).map(|(n, _)| n.clone()).collect();
            let eq = E::Bin("=", bx(E::Col(new[g.index(new.len())].clone())), bx(E::Col(old[g.index(old.len())].clone())));
            if g.bool_with(0.25) { E::Bin("AND", bx(eq), bx(expr(g, &mut sc, Class::Bool, 1))) } else { eq }
        } else {
            expr(g, &mut sc, Class::Bool, 2)
        };
        from.last_mut().unwrap().on = Some(on);
    }
    // SEMI / ANTI hide the right side's columns
    let mut cols = Vec::new();
    for (i, f) in from.iter().enumerate() {
        if f.join == "SEMI JOIN" || f.join == "ANTI JOIN" {
            continue;
        }
        for c in &tables[f.table].cols {
            cols.push((format!("x{i}.{}", c.name), &c.ty));
        }
    }
    let ks: Vec<String> = cols.iter().filter(|(n, _)| n.ends_with(".k")).map(|(n, _)| n.clone()).collect();
    let mut sc = Scope { cols, tables, next_alias: nfrom, allow_sub: true, pushable };
    let wher = if g.bool_with(0.6) { Some(expr(g, &mut sc, Class::Bool, 3)) } else { None };
    sc.allow_sub = false;
    let mut q = Q { distinct: false, items: vec![], base: 0, from, wher, group: None, having: None, order: None, limit: None };
    if g.bool_with(0.35) {
        let n = g.int(0usize..=2);
        for _ in 0..n {
            let c = any_class(g);
            let d = if g.bool_with(0.7) { 0 } else { 2 };
            q.items.push(expr(g, &mut sc, c, d));
        }
        for _ in 0..g.int(1usize..=2) {
            q.items.push(agg(g, &mut sc));
        }
        q.group = Some(n);
        if g.bool_with(0.25) {
            let arg = expr(g, &mut sc, Class::Int, 1);
            let a = if g.bool() { E::Agg("count", false, None) } else { E::Agg(g.pick(&["sum", "min", "max", "count"]), false, Some(bx(arg))) };
            let rhs = lit(g, Class::Int);
            q.having = Some(E::Bin(g.pick(&[">", "<", "=", ">=", "<>"]), bx(a), bx(rhs)));
        }
    } else {
        for _ in 0..g.int(1usize..=4) {
            if !pushable && g.bool_with(0.12) {
                q.items.push(window(g, &mut sc, &ks));
            } else if pushable {
                // a column of any kind, or integer / decimal arithmetic
                let c = if g.bool_with(0.6) { any_class(g) } else { *g.pick(&[Class::Int, Class::Dec]) };
                let d = if matches!(c, Class::Int | Class::Dec) { g.int(0usize..=2) } else { 0 };
                let e = if d == 0 { col(g, &sc, c).unwrap_or(E::Col(ks[0].clone())) } else { expr(g, &mut sc, c, d) };
                q.items.push(e);
            } else {
                let c = any_class(g);
                q.items.push(expr(g, &mut sc, c, 2));
            }
        }
        q.distinct = g.bool_with(0.15);
    }
    if g.bool_with(if pushable { 0.7 } else { 0.5 }) {
        q.order = Some(q.items.iter().map(|_| (g.bool_with(0.3), g.bool_with(0.3))).collect());
        if g.bool_with(if pushable { 0.8 } else { 0.5 }) {
            q.limit = Some((g.int(0u32..=5), if g.bool_with(0.3) { g.int(1u32..=3) } else { 0 }));
        }
    }
    q
}

pub fn case(g: &mut Source) -> Case {
    let nt = g.int(1usize..=3);
    let tables: Vec<Table> = (0..nt).map(|_| table(g)).collect();
    let nq = g.int(2usize..=5);
    let queries = (0..nq).map(|_| query(g, &tables)).collect();
    Case { tables, queries }
}

// ---------------------------------------------------------------- shrink

impl E {
    fn kids(&self) -> Vec<&E> {
        match self {
            E::Col(_) | E::Lit(_) => vec![],
            E::Un(_, a) | E::IsNull(a, _) => vec![a],
            E::Bin(_, a, b) => vec![a, b],
            E::Fn(_, v) => v.iter().collect(),
            E::Case(a, b, c) | E::Between(a, b, c) => vec![a, b, c],
            E::In(a, l, _) => std::iter::once(&**a).chain(l.iter()).collect(),
            E::Agg(_, _, a) => a.iter().map(|a| &**a).collect(),
            E::Win(_, a, p, o) => a.iter().chain(p).chain(o).collect(),
            E::Sub(_, l, _) => l.iter().map(|a| &**a).collect(),
        }
    }
    fn with_kid(&self, i: usize, n: E) -> E {
        let mut e = self.clone();
        match &mut e {
            E::Col(_) | E::Lit(_) => {},
            E::Un(_, a) | E::IsNull(a, _) => **a = n,
            E::Bin(_, a, b) => *(if i == 0 { a } else { b }) = Box::new(n),
            E::Fn(_, v) => v[i] = n,
            E::Case(a, b, c) | E::Between(a, b, c) => *[a, b, c][i] = Box::new(n),
            E::In(a, l, _) => {
                if i == 0 {
                    **a = n
                } else {
                    l[i - 1] = n
                }
            },
            E::Agg(_, _, a) => *a = Some(Box::new(n)),
            E::Win(_, a, p, o) => {
                let (la, lp) = (a.len(), p.len());
                if i < la {
                    a[i] = n
                } else if i < la + lp {
                    p[i - la] = n
                } else {
                    o[i - la - lp] = n
                }
            },
            E::Sub(_, l, _) => *l = Some(Box::new(n)),
        }
        e
    }
}

pub type Visit<'a, T> = &'a mut dyn FnMut(T) -> ControlFlow<()>;

pub fn shrink_e(e: &E, visit: Visit<E>) -> ControlFlow<()> {
    let kids = e.kids();
    // a child replaces the node (not for aggregates and windows: the query's shape depends on them)
    if !matches!(e, E::Agg(..) | E::Win(..)) {
        for k in &kids {
            visit((*k).clone())?;
        }
    }
    match e {
        E::In(a, l, neg) if l.len() > 1 => {
            for i in 0..l.len() {
                let mut l2 = l.clone();
                l2.remove(i);
                visit(E::In(a.clone(), l2, *neg))?;
            }
        },
        E::Win(f, a, p, o) => {
            if !p.is_empty() {
                visit(E::Win(f, a.clone(), vec![], o.clone()))?;
            }
            for i in 0..o.len() {
                let mut o2 = o.clone();
                o2.remove(i);
                visit(E::Win(f, a.clone(), p.clone(), o2))?;
            }
        },
        E::Sub(kind, l, q) => {
            shrink_q(q, &mut |q2| visit(E::Sub(kind, l.clone(), Box::new(q2))))?;
        },
        E::Agg(f, true, a) => visit(E::Agg(f, false, a.clone()))?,
        _ => {},
    }
    for (i, k) in kids.iter().enumerate() {
        shrink_e(k, &mut |n| visit(e.with_kid(i, n)))?;
    }
    ControlFlow::Continue(())
}

pub fn shrink_opt(e: &Option<E>, visit: Visit<Option<E>>) -> ControlFlow<()> {
    if let Some(e) = e {
        visit(None)?;
        shrink_e(e, &mut |n| visit(Some(n)))?;
    }
    ControlFlow::Continue(())
}

fn shrink_q(q: &Q, visit: Visit<Q>) -> ControlFlow<()> {
    macro_rules! with {
        ($($f:ident = $v:expr),*) => {{ let mut n = q.clone(); $(n.$f = $v;)* visit(n)? }};
    }
    if q.limit.is_some() {
        with!(limit = None);
    }
    if q.order.is_some() {
        with!(order = None, limit = None);
    }
    if q.distinct {
        with!(distinct = false);
    }
    shrink_opt(&q.wher, &mut |w| { let mut n = q.clone(); n.wher = w; visit(n) })?;
    shrink_opt(&q.having, &mut |w| { let mut n = q.clone(); n.having = w; visit(n) })?;
    if q.from.len() > 1 {
        let mut n = q.clone();
        n.from.pop();
        visit(n)?;
    }
    if q.items.len() > 1 {
        for i in 0..q.items.len() {
            let mut n = q.clone();
            n.items.remove(i);
            if let Some(o) = &mut n.order {
                o.remove(i);
            }
            if let Some(g) = &mut n.group {
                if i < *g {
                    *g -= 1;
                }
            }
            visit(n)?;
        }
    }
    for (i, f) in q.from.iter().enumerate().skip(1) {
        if f.join != "JOIN" && f.join != "CROSS JOIN" {
            let mut n = q.clone();
            n.from[i].join = "JOIN";
            visit(n)?;
        }
        if let Some(on) = &f.on {
            shrink_e(on, &mut |e| { let mut n = q.clone(); n.from[i].on = Some(e); visit(n) })?;
        }
    }
    for (i, e) in q.items.iter().enumerate() {
        shrink_e(e, &mut |e| { let mut n = q.clone(); n.items[i] = e; visit(n) })?;
    }
    if let Some((l, off)) = q.limit {
        if off > 0 {
            with!(limit = Some((l, 0)));
        }
    }
    ControlFlow::Continue(())
}

pub fn shrink_tables(c: &Case, visit: Visit<Vec<Table>>) -> ControlFlow<()> {
    for (ti, t) in c.tables.iter().enumerate() {
        let n = t.rows.len();
        let mut put = |rows: Vec<Vec<Option<String>>>| {
            let mut ts = c.tables.clone();
            ts[ti].rows = rows;
            visit(ts)
        };
        if n > 0 {
            put(vec![])?;
        }
        if n > 1 {
            put(t.rows[..n / 2].to_vec())?;
            put(t.rows[n / 2..].to_vec())?;
            for i in 0..n {
                let mut r = t.rows.clone();
                r.remove(i);
                put(r)?;
            }
        }
        // a value becomes NULL
        for (ri, row) in t.rows.iter().enumerate() {
            for ci in 1..row.len() {
                if row[ci].is_some() {
                    let mut r = t.rows.clone();
                    r[ri][ci] = None;
                    put(r)?;
                }
            }
        }
        if t.pk {
            let mut ts = c.tables.clone();
            ts[ti].pk = false;
            visit(ts)?;
        }
    }
    ControlFlow::Continue(())
}

pub fn shrink_case(c: &Case, visit: Visit<Case>) -> ControlFlow<()> {
    if c.queries.len() > 1 {
        for q in &c.queries {
            visit(Case { tables: c.tables.clone(), queries: vec![q.clone()] })?;
        }
    }
    shrink_tables(c, &mut |tables| visit(Case { tables, queries: c.queries.clone() }))?;
    for (i, q) in c.queries.iter().enumerate() {
        shrink_q(q, &mut |n| {
            let mut c2 = c.clone();
            c2.queries[i] = n;
            visit(c2)
        })?;
    }
    ControlFlow::Continue(())
}

/// Support predicate of `case`.
pub fn valid_case(c: &Case) -> bool {
    !c.tables.is_empty()
        && c.tables.len() <= 3
        && c.tables.iter().all(|t| t.cols[0].name == "k" && t.rows.iter().all(|r| r.len() == t.cols.len() && r[0].is_some()))
        && c.queries.iter().all(|q| {
            !q.items.is_empty()
                && q.from.iter().all(|f| f.table < c.tables.len())
                && q.order.as_ref().is_none_or(|o| o.len() == q.items.len())
                && q.group.is_none_or(|g| g <= q.items.len())
                && (q.limit.is_none() || q.order.is_some())
        })
}

