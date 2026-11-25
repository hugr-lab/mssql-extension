//! Runs one script in a fresh `duckdb` CLI process and splits its output by markers.
use std::io::{Read, Write};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Debug, Default, Clone)]
pub struct Section {
    pub rows: Vec<String>,
    /// (exception_type, message)
    pub errs: Vec<(String, String)>,
}

#[derive(Debug, PartialEq)]
pub enum Status {
    Exited,
    Signal(String),
    Timeout,
}

#[derive(Debug)]
pub struct Out {
    pub pre: Section,
    pub sections: Vec<Section>,
    pub status: Status,
    pub script: String,
}

pub fn env(name: &str, default: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| default.to_string())
}

pub fn db() -> String {
    env("EPH_DB", "ephyra0")
}

pub fn dsn(db: &str) -> String {
    format!(
        "Server={},{};Database={db};User Id={};Password={};TrustServerCertificate=yes",
        env("MSSQL_TEST_HOST", "localhost"),
        env("MSSQL_TEST_PORT", "14330"),
        env("MSSQL_TEST_USER", "sa"),
        env("MSSQL_TEST_PASS", "TestPassword1")
    )
}

pub fn lit(s: &str) -> String {
    format!("'{}'", s.replace('\'', "''"))
}

#[derive(Default)]
pub struct Script {
    body: String,
    sections: usize,
}

impl Script {
    pub fn new() -> Self {
        let mut s = Script::default();
        s.body += ".bail off\n.mode jsonlines\n";
        s
    }
    /// A statement whose output is not compared (errors land in `pre` or the current section).
    pub fn stmt(&mut self, sql: &str) -> &mut Self {
        self.body += sql.trim_end_matches(';');
        self.body += ";\n";
        self
    }
    /// A statement with its own section; returns the section's index.
    pub fn section(&mut self, sql: &str) -> usize {
        if self.sections == 0 {
            self.body += "SELECT error('@@pre');\n";
        }
        let n = self.sections;
        self.sections += 1;
        self.body += &format!(".print @@{n}\n");
        self.stmt(sql);
        self.body += &format!("SELECT error('@@{n}');\n");
        n
    }
    /// A section with no error marker (a marker is an error, and an error ends an open
    /// transaction): its errors land in the next `section`.
    pub fn section_quiet(&mut self, sql: &str) -> usize {
        if self.sections == 0 {
            self.body += "SELECT error('@@pre');\n";
        }
        let n = self.sections;
        self.sections += 1;
        self.body += &format!(".print @@{n}\n");
        self.stmt(sql);
        n
    }
    pub fn run(&self) -> Out {
        run(&self.body, self.sections)
    }
}

fn duckdb() -> String {
    env("EPH_DUCKDB", concat!(env!("CARGO_MANIFEST_DIR"), "/../../build/release/duckdb"))
}

pub fn run(script: &str, nsections: usize) -> Out {
    let mut child = Command::new(duckdb())
        .arg("-unsigned")
        .env("AI_AGENT", "1")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawn duckdb");
    let mut stdin = child.stdin.take().unwrap();
    let text = script.to_string();
    let writer = std::thread::spawn(move || {
        let _ = stdin.write_all(text.as_bytes());
    });
    let mut so = child.stdout.take().unwrap();
    let mut se = child.stderr.take().unwrap();
    let t_out = std::thread::spawn(move || {
        let mut s = Vec::new();
        let _ = so.read_to_end(&mut s);
        String::from_utf8_lossy(&s).into_owned()
    });
    let t_err = std::thread::spawn(move || {
        let mut s = Vec::new();
        let _ = se.read_to_end(&mut s);
        String::from_utf8_lossy(&s).into_owned()
    });
    let limit = Duration::from_secs(env("EPH_TIMEOUT_S", "90").parse().unwrap_or(90));
    let start = Instant::now();
    let status = loop {
        match child.try_wait().expect("wait") {
            Some(st) => {
                #[cfg(unix)]
                {
                    use std::os::unix::process::ExitStatusExt;
                    if let Some(sig) = st.signal() {
                        break Status::Signal(format!("signal {sig}"));
                    }
                }
                let _ = st;
                break Status::Exited;
            },
            None if start.elapsed() > limit => {
                let _ = child.kill();
                let _ = child.wait();
                break Status::Timeout;
            },
            None => std::thread::sleep(Duration::from_millis(3)),
        }
    };
    let _ = writer.join();
    let stdout = t_out.join().unwrap();
    let stderr = t_err.join().unwrap();

    let mut pre = Section::default();
    let mut sections = vec![Section::default(); nsections];
    let mut cur: Option<usize> = None;
    for line in stdout.lines() {
        // the CLI prints an empty line for some empty results
        if line.is_empty() {
            continue;
        }
        if let Some(n) = line.strip_prefix("@@").and_then(|n| n.parse::<usize>().ok()) {
            cur = Some(n);
            continue;
        }
        match cur {
            Some(n) if n < nsections => sections[n].rows.push(line.to_string()),
            _ => pre.rows.push(line.to_string()),
        }
    }
    // stderr: errors belong to the section whose end marker follows them
    let mut pending: Vec<(String, String)> = Vec::new();
    #[allow(unused_assignments)]
    let mut first = true;
    for line in stderr.lines() {
        let (ty, msg) = match serde_json::from_str::<serde_json::Value>(line) {
            Ok(v) if v.get("exception_type").is_some() => {
                (v["exception_type"].as_str().unwrap_or("").to_string(), v["exception_message"].as_str().unwrap_or("").to_string())
            },
            _ if line.starts_with("duckdb agent mode") => continue,
            _ => ("RAW".to_string(), line.to_string()),
        };
        if msg == "@@pre" {
            pre.errs.append(&mut pending);
            first = false;
            continue;
        }
        if let Some(n) = msg.strip_prefix("@@").and_then(|n| n.parse::<usize>().ok()) {
            let errs = std::mem::take(&mut pending);
            if n < nsections {
                sections[n].errs = errs;
            }
            continue;
        }
        pending.push((ty, msg));
    }
    // errors after the last marker (a crash mid-section) stay visible
    pre.errs.extend(pending);
    Out { pre, sections, status, script: script.to_string() }
}

/// The first words of a message, without values: a stable signature.
pub fn words(msg: &str, n: usize) -> String {
    msg.split(|c: char| !c.is_ascii_alphabetic()).filter(|w| w.len() > 1).take(n).collect::<Vec<_>>().join("_").to_lowercase()
}

/// A crash-oracle verdict for a whole run: Some(signature) on INTERNAL / FATAL / signal / hang.
pub fn crash(out: &Out) -> Option<String> {
    match &out.status {
        Status::Signal(s) => return Some(format!("crash:{s}")),
        Status::Timeout => return Some("crash:hang".into()),
        Status::Exited => {},
    }
    for s in std::iter::once(&out.pre).chain(&out.sections) {
        for (ty, msg) in &s.errs {
            let t = ty.to_uppercase();
            if t == "INTERNAL" || t == "FATAL" || t == "RAW" {
                return Some(format!("crash:{}:{}", t.to_lowercase(), words(msg, 8)));
            }
        }
    }
    None
}
