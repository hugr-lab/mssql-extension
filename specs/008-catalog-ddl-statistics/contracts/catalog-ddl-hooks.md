# Contract: Catalog DDL Hooks

**Feature**: 008-catalog-ddl-statistics
**Modules**: `mssql_catalog`, `mssql_schema_entry`

## Purpose

Defines the DuckDB catalog hook methods that intercept DDL operations and translate them to T-SQL for execution on SQL Server.

## MSSQLCatalog Interface

```cpp
namespace duckdb {

//! Access mode for attached MSSQL catalog
enum class MSSQLAccessMode : uint8_t {
    READ_WRITE = 0,  //! Default - DDL and mssql_exec allowed
    READ_ONLY = 1    //! SELECT only - DDL and mssql_exec blocked
};

class MSSQLCatalog : public Catalog {
public:
    //! Constructor
    //! @param db DuckDB database
    //! @param name Catalog name
    //! @param connection_info Connection parameters
    //! @param access_mode Read-write or read-only
    MSSQLCatalog(DatabaseInstance &db,
                 const string &name,
                 MSSQLConnectionInfo connection_info,
                 MSSQLAccessMode access_mode = MSSQLAccessMode::READ_WRITE);

    //! Check if catalog is read-only
    //! @return True if attached with READ_ONLY parameter
    bool IsReadOnly() const;

    //! Check write access and throw if read-only
    //! @throws CatalogException if catalog is read-only
    void CheckWriteAccess() const;

    // ─────────────────────────────────────────────────────────────────
    // Schema DDL Hooks (override from Catalog)
    // ─────────────────────────────────────────────────────────────────

    //! Create a new schema on SQL Server
    //! @param transaction Catalog transaction
    //! @param info Schema creation info
    //! @return Created schema entry or nullptr
    optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction,
                                            CreateSchemaInfo &info) override;

    //! Drop a schema from SQL Server
    //! @param context Client context
    //! @param info Drop info with schema name
    void DropSchema(ClientContext &context, DropInfo &info) override;

    // ─────────────────────────────────────────────────────────────────
    // Statistics Integration
    // ─────────────────────────────────────────────────────────────────

    //! Get statistics provider instance
    //! @return Reference to statistics provider
    MSSQLStatisticsProvider &GetStatisticsProvider();

private:
    //! Access mode (read-write or read-only)
    MSSQLAccessMode access_mode_;

    //! Statistics provider
    unique_ptr<MSSQLStatisticsProvider> statistics_provider_;

    //! Execute DDL T-SQL on SQL Server
    //! @param context Client context
    //! @param sql T-SQL statement to execute
    //! @param operation Operation type for error messages
    void ExecuteDDL(ClientContext &context,
                    const string &sql,
                    const string &operation);

    //! Invalidate metadata cache for schema
    //! @param schema_name Schema that was modified
    void InvalidateSchemaCache(const string &schema_name);
};

} // namespace duckdb
```

## MSSQLSchemaEntry Interface

```cpp
namespace duckdb {

class MSSQLSchemaEntry : public SchemaCatalogEntry {
public:
    //! Constructor
    MSSQLSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);

    // ─────────────────────────────────────────────────────────────────
    // Table DDL Hooks (override from SchemaCatalogEntry)
    // ─────────────────────────────────────────────────────────────────

    //! Create a new table on SQL Server
    //! @param transaction Catalog transaction
    //! @param info Bound table creation info with columns
    //! @return Created table entry or nullptr
    optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction,
                                           BoundCreateTableInfo &info) override;

    //! Drop a table or view from SQL Server
    //! @param context Client context
    //! @param info Drop info with table/view name
    void DropEntry(ClientContext &context, DropInfo &info) override;

    //! Alter a table on SQL Server
    //! @param transaction Catalog transaction
    //! @param info Alter info (polymorphic)
    void Alter(CatalogTransaction transaction, AlterInfo &info) override;

private:
    //! Get parent catalog as MSSQLCatalog
    MSSQLCatalog &GetMSSQLCatalog();

    //! Handle RenameTableInfo
    void AlterRenameTable(ClientContext &context, RenameTableInfo &info);

    //! Handle RenameColumnInfo
    void AlterRenameColumn(ClientContext &context, RenameColumnInfo &info);

    //! Handle AddColumnInfo
    void AlterAddColumn(ClientContext &context, AddColumnInfo &info);

    //! Handle RemoveColumnInfo
    void AlterRemoveColumn(ClientContext &context, RemoveColumnInfo &info);

    //! Handle ChangeColumnTypeInfo
    void AlterChangeColumnType(ClientContext &context, ChangeColumnTypeInfo &info);

    //! Handle SetNotNullInfo
    void AlterSetNotNull(ClientContext &context, SetNotNullInfo &info);

    //! Handle DropNotNullInfo
    void AlterDropNotNull(ClientContext &context, DropNotNullInfo &info);
};

} // namespace duckdb
```

## AlterInfo Dispatch Table

| AlterInfo Type | Method | T-SQL Pattern |
| -------------- | ------ | ------------- |
| RenameTableInfo | AlterRenameTable | `sp_rename` |
| RenameColumnInfo | AlterRenameColumn | `sp_rename ... COLUMN` |
| AddColumnInfo | AlterAddColumn | `ALTER TABLE ... ADD` |
| RemoveColumnInfo | AlterRemoveColumn | `ALTER TABLE ... DROP COLUMN` |
| ChangeColumnTypeInfo | AlterChangeColumnType | `ALTER TABLE ... ALTER COLUMN` |
| SetNotNullInfo | AlterSetNotNull | `ALTER TABLE ... ALTER COLUMN ... NOT NULL` |
| DropNotNullInfo | AlterDropNotNull | `ALTER TABLE ... ALTER COLUMN ... NULL` |

## Error Format

```
MSSQL DDL error (<operation>): [<schema>].[<object>]
SQL Server error <number>, state <state>, class <severity>: <message>
```

Example:
```
MSSQL DDL error (CREATE_TABLE): [dbo].[users]
SQL Server error 2714, state 6, class 16: There is already an object named 'users' in the database.
```

## Read-Only Mode

When `access_mode_ == MSSQLAccessMode::READ_ONLY`:

1. All DDL hooks call `CheckWriteAccess()` first
2. `CheckWriteAccess()` throws: `CatalogException("Cannot modify MSSQL catalog '[name]': attached in read-only mode")`
3. No T-SQL is sent to SQL Server
4. SELECT queries continue to work normally

## Cache Invalidation Flow

```
DDL Operation
     │
     ▼
CheckWriteAccess()
     │
     ▼
Generate T-SQL (MSSQLDDLTranslator)
     │
     ▼
Execute on SQL Server
     │
     ├─── Success ───► InvalidateCache()
     │
     └─── Failure ───► Throw Exception (cache unchanged)
```

## Thread Safety

- DDL operations are serialized per catalog (DuckDB transaction model)
- Cache invalidation is atomic
- Statistics provider handles its own thread safety
