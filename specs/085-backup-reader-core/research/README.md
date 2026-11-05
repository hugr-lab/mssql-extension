# Research behind specs 085–090

The research record for reading SQL Server backups without SQL Server and importing them into DuckLake.
Start with [RESEARCH.md](RESEARCH.md): it is the design synthesis and carries every owner decision (marked
"owner, 2026-11-05"). The specs distil it: [series overview](../series.md), [decisions](../decisions.md).

| file | what |
|---|---|
| [RESEARCH.md](RESEARCH.md) | the design synthesis, decisions, phases, experiment plan |
| [research-web.md](research-web.md) | formats, prior art and licences (public sources) |
| [research-experiment.md](research-experiment.md) | first decode of real SQL Server 2025 backups |
| [research-architecture.md](research-architecture.md) | fit with mssql and mssql-ducklake |
| [research-ducklake-import.md](research-ducklake-import.md) | the DuckLake import pipeline (Parquet first, one commit) |
| [experiments/](experiments/) | E1–E7: lake as base, allocation edges, fuzzy backups, encryption / TDE, columnstore, exact healing, partitioning |

The probe scripts and sample backups these reports cite (`tools/…`, `experiments/E<n>/…`, `samples/`) stay in the
maintainers' local research folder (`design/001-mssql-backup-reader/`, git-ignored): the backups run to
gigabytes, and their pad pages hold stale server memory. Everything here was produced clean-room from our own
experiments, public specifications (MTF, MS-XCA, MS-SSCLRT, MS-BINXML) and MIT-licensed sources.
