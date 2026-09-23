# Transaction Management

The transaction system maps DuckDB's transaction semantics to SQL Server's TDS protocol, using connection pinning to ensure all operations within a transaction execute on the same SQL Server connection.

## Classes

### MSSQLTransaction

**Files**: `src/catalog/mssql_transaction.cpp`, `src/include/catalog/mssql_transaction.hpp`

Extends DuckDB's `Transaction` base class. One instance per explicit DuckDB transaction per attached MSSQL database.

**Key state**:

```cpp
std::shared_ptr<tds::TdsConnection> pinned_connection_;   // Pinned for transaction duration
mutable mutex connection_mutex_;                            // Serialize operations
bool sql_server_transaction_active_;                        // BEGIN TRANSACTION sent?
uint8_t transaction_descriptor_[8];                         // From ENVCHANGE
bool has_transaction_descriptor_;
uint32_t savepoint_counter_;                                // For future savepoint support
```

**Key methods**:

| Method | Purpose |
|---|---|
| `GetPinnedConnection()` | Return pinned connection (may be nullptr) |
| `HasPinnedConnection()` | Check if connection is pinned |
| `SetPinnedConnection(conn)` | Pin connection on first DML |
| `IsSqlServerTransactionActive()` | Check if BEGIN TRANSACTION sent |
| `SetSqlServerTransactionActive(bool)` | Update SQL Server transaction state |
| `GetTransactionDescriptor()` | Get 8-byte descriptor (nullptr if not set) |
| `SetTransactionDescriptor(desc)` | Store descriptor from ENVCHANGE |
| `GetNextSavepointName()` | Generate unique savepoint names |

**Destructor behavior**: If transaction is abandoned with an active SQL Server transaction, the destructor closes the connection. SQL Server automatically rolls back when a connection closes.

### MSSQLTransactionManager

**Files**: `src/catalog/mssql_transaction.cpp`, `src/include/catalog/mssql_transaction.hpp`

Extends DuckDB's `TransactionManager`. Creates and manages `MSSQLTransaction` instances.

**Key methods**:

| Method | Behavior |
|---|---|
| `StartTransaction(context)` | Create new MSSQLTransaction for the context |
| `CommitTransaction(context, txn)` | Send `COMMIT TRANSACTION`, return connection to pool |
| `RollbackTransaction(txn)` | Send `ROLLBACK TRANSACTION`, return connection to pool |
| `Checkpoint(context, force)` | No-op (remote database, no local checkpoint) |

**State tracking**:

```cpp
MSSQLCatalog &catalog_;
mutex transaction_lock_;
reference_map_t<ClientContext, unique_ptr<MSSQLTransaction>> transactions_;
```

## Autocommit vs Explicit Transaction Mode

### Autocommit Mode (Default)

When no explicit `BEGIN` is issued, each statement is independent:

1. `ConnectionProvider::GetConnection()` acquires from pool (no pinning)
2. `ExecuteBatch()` sends SQL with zero transaction descriptor in ALL_HEADERS
3. SQL Server treats each statement as its own implicit transaction
4. Connection returns to pool immediately after statement completes
5. No `MSSQLTransaction` object involved

One exception, since spec 062: a DML statement that the extension sends as
SEVERAL requests — the batches of an INSERT, UPDATE or DELETE, or the
INSERT BULK batches of a bulk-path INSERT — brackets them in a server
transaction of its own (`mssql::LoadTransaction`, `copy/load_transaction.hpp`):
`BEGIN TRANSACTION` on the statement's connection before the first request,
`COMMIT TRANSACTION` after the last, `ROLLBACK` on any failure. The
transaction descriptor the server answers with rides in ALL_HEADERS exactly
as for a pinned connection, and is cleared at the commit. So an autocommit
DML statement is atomic (issue #344), and holds its one pool connection for
its whole duration. Inside an explicit transaction the helper is a no-op —
the pinned connection's transaction already spans every request.

### Explicit Transaction Mode

After `BEGIN` is issued:

1. DuckDB calls `MSSQLTransactionManager::StartTransaction()` → creates `MSSQLTransaction`
2. First DML/scan triggers `ConnectionProvider::GetConnection()`
3. Connection acquired from pool and pinned to transaction
4. `BEGIN TRANSACTION` sent to SQL Server
5. ENVCHANGE response parsed for 8-byte transaction descriptor
6. All subsequent operations reuse the pinned connection with descriptor in ALL_HEADERS
7. `COMMIT` or `ROLLBACK` releases the connection back to pool

## Connection Pinning

Connection pinning ensures all operations within a DuckDB transaction execute on the same SQL Server connection with the same transaction context.

### Why Pinning is Needed

SQL Server identifies transactions by the connection they were started on. The 8-byte transaction descriptor from the ENVCHANGE token must be included in every subsequent SQL_BATCH packet's ALL_HEADERS structure. Using a different connection would route the request outside the transaction.

### Pinning Flow

```
BEGIN; (DuckDB)
  │
  ▼ StartTransaction() → MSSQLTransaction created (no pinned connection yet)

INSERT INTO mydb.dbo.t VALUES (1); (first DML)
  │
  ▼ ConnectionProvider::GetConnection()
     ├─ No pinned connection
     ├─ Acquire from pool
     ├─ txn->SetPinnedConnection(conn)
     ├─ Execute "BEGIN TRANSACTION"
     ├─ Parse ENVCHANGE → extract descriptor
     ├─ txn->SetTransactionDescriptor(desc)
     ├─ conn->SetTransactionDescriptor(desc)
     └─ Return pinned connection

UPDATE mydb.dbo.t SET x = 2; (subsequent DML)
  │
  ▼ ConnectionProvider::GetConnection()
     ├─ txn->HasPinnedConnection() == true
     └─ Return same pinned connection (with descriptor in ALL_HEADERS)

COMMIT; (or ROLLBACK)
  │
  ▼ CommitTransaction() / RollbackTransaction()
     ├─ Execute "COMMIT TRANSACTION" / "ROLLBACK TRANSACTION"
     ├─ conn->ClearTransactionDescriptor()
     ├─ Return connection to pool
     └─ Clear pinned connection from transaction
```

## Transaction Descriptor (8-byte ENVCHANGE)

### What Is It?

An opaque 8-byte identifier returned by SQL Server in the ENVCHANGE token after `BEGIN TRANSACTION`. It uniquely identifies the transaction on the server side.

### ENVCHANGE Token Structure

```
Token: 0xE3 (ENVCHANGE)
Length: 2 bytes LE
Type:   1 byte = 0x08 (BEGIN_TRANS)
NewLen: 1 byte = 0x08 (8 bytes)
NewVal: 8 bytes = transaction descriptor
OldLen: 1 byte
```

### ALL_HEADERS in SQL_BATCH

Every SQL_BATCH packet includes an ALL_HEADERS structure with the transaction descriptor:

```
TotalLength:              4 bytes LE (22)
HeaderLength:             4 bytes LE (18)
HeaderType:               2 bytes LE (0x0002 = Transaction Descriptor)
TransactionDescriptor:    8 bytes (from ENVCHANGE, or 8 zero bytes if no transaction)
OutstandingRequestCount:  4 bytes LE (1)
```

Without the correct descriptor, SQL Server cannot route a SQL_BATCH to the active transaction.

## BEGIN / COMMIT / ROLLBACK Flow

### BEGIN TRANSACTION

```
User: BEGIN;
  ▼
MSSQLTransactionManager::StartTransaction()
  ├─ Create MSSQLTransaction
  └─ Store in transactions_ map keyed by ClientContext

(Deferred until first DML operation)

First DML:
  ▼
ConnectionProvider::GetConnection()
  ├─ Acquire connection from pool
  ├─ Pin to transaction
  ├─ conn->ExecuteBatch("BEGIN TRANSACTION")
  ├─ Receive response
  ├─ Parse tokens, find ENVCHANGE type 0x08
  ├─ Extract 8-byte descriptor
  ├─ Store in transaction and connection
  └─ Mark sql_server_transaction_active_ = true
```

### COMMIT TRANSACTION

```
User: COMMIT;
  ▼
MSSQLTransactionManager::CommitTransaction()
  ├─ Get MSSQLTransaction for context
  ├─ Check: HasPinnedConnection() && IsSqlServerTransactionActive()
  │
  ├─ YES:
  │   ├─ ExecuteAndDrain("COMMIT TRANSACTION")
  │   ├─ On failure: return ErrorData (user must rollback)
  │   ├─ On success:
  │   │   ├─ Clear SQL Server transaction state
  │   │   ├─ conn->ClearTransactionDescriptor()
  │   │   ├─ conn->SetNeedsReset(true) (RESET_CONNECTION on next batch)
  │   │   ├─ Return connection to pool
  │   │   └─ Clear pinned connection
  │   └─ Remove from transactions_ map
  │
  └─ NO (no active transaction):
      └─ Remove from transactions_ map (no-op)
```

### ROLLBACK TRANSACTION

```
User: ROLLBACK;
  ▼
MSSQLTransactionManager::RollbackTransaction()
  ├─ Get MSSQLTransaction
  ├─ Check: HasPinnedConnection() && IsSqlServerTransactionActive()
  │
  ├─ YES:
  │   ├─ ExecuteAndDrain("ROLLBACK TRANSACTION")
  │   ├─ Log errors as WARNING but continue cleanup
  │   ├─ Clear SQL Server transaction state
  │   ├─ conn->ClearTransactionDescriptor()
  │   ├─ conn->SetNeedsReset(true) (RESET_CONNECTION on next batch)
  │   ├─ Return connection to pool
  │   └─ Clear pinned connection
  │
  └─ NO:
      └─ No-op
  │
  └─ Try to remove from transactions_ map
```

**Key difference from COMMIT**: Rollback errors are logged but do not prevent cleanup. This ensures the connection always returns to the pool.

### ExecuteAndDrain Helper

Used by both COMMIT and ROLLBACK to execute the transaction SQL and consume the complete response:

```cpp
bool ExecuteAndDrain(TdsConnection &conn, const string &sql, int timeout_ms = 5000) {
    if (!conn.ExecuteBatch(sql)) return false;
    auto *socket = conn.GetSocket();
    if (!socket) return false;
    vector<uint8_t> response;
    if (!socket->ReceiveMessage(response, timeout_ms)) return false;
    conn.TransitionState(ConnectionState::Executing, ConnectionState::Idle);
    return true;
}
```

## SQL Server Isolation Behavior

Unless the `transaction_isolation` option says otherwise, the extension sends no `SET TRANSACTION ISOLATION LEVEL`. The server's default applies, which is **READ COMMITTED**.

### Why the level matters here

Each scan of a DuckDB transaction is its own statement on the pinned connection. Under READ COMMITTED, a row that another session commits between two scans is seen by the second scan and not by the first. A query that reads the same table twice can therefore see two states of it. DuckLake's commit-conflict check does exactly that (issue #331).

Measured with two reads inside one transaction and a committed insert from another session between them:

| Level | First read / second read |
|---|---|
| READ COMMITTED | 3 / 4 |
| SNAPSHOT | 3 / 3 |

`READ_COMMITTED_SNAPSHOT` is not enough: it is consistent per statement, and the two scans are two statements.

### The option

`transaction_isolation` can be an ATTACH option, `TransactionIsolation=` in a connection string, `transaction_isolation=` in a URI, or a secret parameter. The ATTACH option wins. The value is one of:

- **`default`** (or unset): send nothing, as before.
- **`read_uncommitted`, `read_committed`, `repeatable_read`, `serializable`, `snapshot`**: sent as `SET TRANSACTION ISOLATION LEVEL …` on the pinned connection, as its own statement, before `BEGIN TRANSACTION`. The SET must come before BEGIN, because a transaction cannot switch to SNAPSHOT once it has started.
- **`auto`**: SNAPSHOT when the database has `ALLOW_SNAPSHOT_ISOLATION ON`, and nothing otherwise, including when that could not be determined.
  - The state is probed at ATTACH: `sys.databases.snapshot_isolation_state`, in the same query as the database collation, so it costs no extra round trip.
  - The probe runs only when the option is `snapshot` or `auto`, and not on Fabric or Synapse. Every other ATTACH sends the same query as before, so a platform whose `sys.databases` lacks the column cannot fail an ATTACH that never asked about isolation.
  - A missing row reads as unknown, and unknown falls back to nothing.

An explicit `snapshot` against a database where snapshot isolation is OFF is refused at ATTACH. Otherwise BEGIN would succeed and the first read of a user table in the transaction would fail with error 3952.

Behaviour on cloud platforms:

- **Fabric Warehouse** gets nothing whatever was asked: it enforces snapshot isolation on every transaction and ignores the SET.
- **Azure Synapse** gets nothing: its only settable level, READ UNCOMMITTED, is already its default. Any other explicit level is refused at ATTACH.

### What SNAPSHOT changes, measured

- **Write conflicts.** Updating or deleting a row that another session changed since the transaction began fails with error 3960: `Snapshot isolation transaction aborted due to update conflict … Retry the transaction`. SQL Server aborts the transaction. Under READ COMMITTED the same UPDATE simply re-reads the row. A caller that retries on "conflict", as DuckLake does, handles this.
- **Bulk loads.** COPY and INSERT through `INSERT BULK` inside a SNAPSHOT transaction work, including TABLOCK on a heap, and their rows are visible in the same transaction.
- **Concurrent readers.** While another session bulk-loads a heap under TABLOCK:
  - a READ COMMITTED reader of that heap **waits** for the load's commit;
  - a SNAPSHOT reader does not wait, and sees the committed state.

### The level is put back

SQL Server keeps a session's isolation level across the `RESET_CONNECTION` the pool sends on reuse. This was measured: SNAPSHOT was still in force on the next autocommit statement although the reset had run and dropped the session's `#temp` table.

So when the transaction set a level, COMMIT and ROLLBACK are followed by `SET TRANSACTION ISOLATION LEVEL READ COMMITTED` before the connection returns to the pool.

- **Why it is a separate statement.** Appended to the ROLLBACK batch, it does not run when the server has already aborted the transaction on a conflict: that batch goes out with a stale transaction descriptor.
- **When it fails.** A connection whose level could not be put back is closed, never pooled.

The same persistence applies to a `SET TRANSACTION ISOLATION LEVEL` sent through `mssql_exec()`. In autocommit it stays on that pooled connection for every later statement, so use the option instead.

## Connection Pool Interaction

| Scenario | Pool Behavior |
|---|---|
| Autocommit statement | Acquire → use → return to pool |
| Transaction first DML | Acquire from pool → pin to transaction |
| Transaction subsequent DML | Reuse pinned connection (no pool interaction) |
| Transaction COMMIT/ROLLBACK | Return pinned connection to pool |
| Abandoned transaction (destructor) | Close connection (not returned to pool) |

### Catalog metadata inside a transaction (issue #380)

Every metadata load a transaction needs — a table's columns, a schema's table
list, the schema list, a rowid key — goes on its **pinned** connection. A pool
connection would not see the transaction's uncommitted DDL and waits on its
schema locks: a table COPY created inside the transaction, read in the same
transaction, used to hang until `mssql_metadata_timeout`. On a pool of one it
had no second connection to take at all.

What such a load finds belongs to the transaction, so it is kept in the
transaction's own cache (`MSSQLTransactionMetadata`), not the catalog's shared
one: another connection must not find an uncommitted table there, and a
ROLLBACK must not leave one behind. The shared cache is still read for names
the transaction did not change, and at COMMIT or ROLLBACK it forgets the names
the transaction changed. See `DATAMODEL.md`, "Inside an explicit transaction".

Inside a transaction:

| Call | |
|---|---|
| `mssql_invalidate_cache()` | allowed — the transaction reloads those names on its pinned connection |
| `mssql_refresh_cache()`, `mssql_preload_catalog()` | refused — bulk loads into the shared cache; run them after COMMIT |

**What still runs outside the transaction:** catalog DDL (`CREATE TABLE`,
`DROP TABLE`, `ALTER TABLE` through DuckDB) and CTAS go on a pool connection and
autocommit, so ROLLBACK does not undo them (spec 057; the undo for a CTAS is
`mssql_ctas_drop_on_failure`). The exception is a pool of **one** connection,
where there is no second connection: a CTAS then runs whole on the pinned one —
checks, CREATE and rows, the rows as INSERT statements — and ROLLBACK undoes it.
DDL sent through `mssql_exec()` has always run on the pinned connection and
rolls back.

### Parallel Transactions

Multiple DuckDB connections can have concurrent transactions against the same SQL Server. Each gets its own pinned TDS connection from the pool. SQL Server maintains isolation between them via its normal locking mechanisms.

## Error Handling

| Scenario | Behavior |
|---|---|
| BEGIN TRANSACTION fails | Clear pinned connection, return to pool, throw IOException |
| COMMIT fails | Return ErrorData, connection stays pinned (user must ROLLBACK) |
| ROLLBACK fails | Log warning, continue cleanup, return connection to pool |
| Transaction abandoned | Destructor closes connection (SQL Server auto-rollback) |
| Connection dies mid-transaction | IOException on next operation |

## State Transition Diagram

```
[Created]                           MSSQLTransactionManager::StartTransaction()
  │ (no pinned connection)
  │
  │ First DML → ConnectionProvider::GetConnection()
  ▼
[Pinned + Active]                   BEGIN TRANSACTION sent, descriptor stored
  │ (pinned_connection_ set)
  │ (sql_server_transaction_active_ = true)
  │
  ├──── COMMIT ──→ [Committed]     Connection returned to pool
  │                                 Descriptor cleared
  │
  └──── ROLLBACK ──→ [Rolled Back] Connection returned to pool
                                    Descriptor cleared

  Both end with:
  └──→ [Destroyed]                  ~MSSQLTransaction()
```

## Debug Logging

Set `MSSQL_DEBUG=1` environment variable to enable transaction logging:

```
[MSSQL_TXN] StartTransaction: context=0x7f..., is_autocommit=0
[MSSQL_TXN] Pinned connection set for transaction
[MSSQL_TXN] SQL Server transaction active: true
[MSSQL_TXN] Transaction descriptor set: 01 02 03 04 05 06 07 08
[MSSQL_CONN] GetConnection: Explicit transaction mode
[MSSQL_CONN] GetConnection: SQL Server transaction started, connection pinned
[MSSQL_TXN] CommitTransaction: Committing SQL Server transaction
```
