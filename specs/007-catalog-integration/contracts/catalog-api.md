# Contract: Catalog API

**Branch**: `007-catalog-integration`
**Date**: 2026-01-18

This contract defines the DuckDB catalog integration interfaces for the MSSQL extension.

---

## C++ Class Interfaces

### MSSQLCatalog

```cpp
namespace duckdb {

class MSSQLCatalog : public Catalog {
public:
    // Constructor
    MSSQLCatalog(AttachedDatabase &db,
                 const MSSQLConnectionInfo &connection_info,
                 AccessMode access_mode);

    ~MSSQLCatalog() override;

    // Required Catalog overrides
    void Initialize(bool load_builtin) override;

    string GetCatalogType() override;        // Returns "mssql"

    optional_ptr<SchemaCatalogEntry> LookupSchema(
        CatalogTransaction transaction,
        const string &schema_name) override;

    void ScanSchemas(
        CatalogTransaction transaction,
        std::function<void(SchemaCatalogEntry &)> callback) override;

    optional_ptr<SchemaCatalogEntry> CreateSchema(
        CatalogTransaction transaction,
        CreateSchemaInfo &info) override;    // Throws - writes not supported

    void DropSchema(
        CatalogTransaction transaction,
        DropInfo &info) override;            // Throws - writes not supported

    // DML planning (all throw - writes not supported)
    unique_ptr<PhysicalOperator> PlanInsert(
        ClientContext &context,
        LogicalInsert &op,
        unique_ptr<PhysicalOperator> plan) override;

    unique_ptr<PhysicalOperator> PlanUpdate(
        ClientContext &context,
        LogicalUpdate &op,
        unique_ptr<PhysicalOperator> plan) override;

    unique_ptr<PhysicalOperator> PlanDelete(
        ClientContext &context,
        LogicalDelete &op,
        unique_ptr<PhysicalOperator> plan) override;

    // Catalog information
    string GetDefaultSchema() override;      // Returns "dbo"
    bool InMemory() override;                // Returns false
    string GetDBPath() override;
    idx_t GetDatabaseSize(ClientContext &context) override;

    // MSSQL-specific accessors
    tds::ConnectionPool &GetConnectionPool();
    MSSQLMetadataCache &GetMetadataCache();
    const string &GetDatabaseCollation() const;

private:
    AttachedDatabase &db_;
    MSSQLConnectionInfo connection_info_;
    AccessMode access_mode_;
    shared_ptr<tds::ConnectionPool> connection_pool_;
    unique_ptr<MSSQLMetadataCache> metadata_cache_;
    string database_collation_;
    MSSQLSchemaSet schemas_;
};

} // namespace duckdb
```

### MSSQLSchemaEntry

```cpp
namespace duckdb {

class MSSQLSchemaEntry : public SchemaCatalogEntry {
public:
    MSSQLSchemaEntry(Catalog &catalog, const string &name);

    ~MSSQLSchemaEntry() override;

    // Required overrides
    optional_ptr<CatalogEntry> CreateTable(
        CatalogTransaction transaction,
        BoundCreateTableInfo &info) override;    // Throws

    optional_ptr<CatalogEntry> LookupEntry(
        CatalogTransaction transaction,
        CatalogType type,
        const string &name) override;

    void Scan(
        CatalogType type,
        std::function<void(CatalogEntry &)> callback) override;

    void DropEntry(
        CatalogTransaction transaction,
        DropInfo &info) override;                 // Throws

    void Alter(
        CatalogTransaction transaction,
        AlterInfo &info) override;                // Throws

    // MSSQL-specific
    MSSQLCatalog &GetMSSQLCatalog();

private:
    MSSQLTableSet tables_;
};

} // namespace duckdb
```

### MSSQLTableEntry

```cpp
namespace duckdb {

class MSSQLTableEntry : public TableCatalogEntry {
public:
    MSSQLTableEntry(Catalog &catalog,
                    SchemaCatalogEntry &schema,
                    const MSSQLTableInfo &table_info);

    ~MSSQLTableEntry() override;

    // Required overrides
    TableFunction GetScanFunction(
        ClientContext &context,
        unique_ptr<FunctionData> &bind_data) override;

    unique_ptr<BaseStatistics> GetStatistics(
        ClientContext &context,
        column_t column_id) override;

    TableStorageInfo GetStorageInfo(
        ClientContext &context) override;

    void BindUpdateConstraints(
        Binder &binder,
        LogicalGet &get,
        LogicalProjection &proj,
        LogicalUpdate &update,
        ClientContext &context) override;     // Throws - writes not supported

    // MSSQL-specific accessors
    const vector<MSSQLColumnInfo> &GetMSSQLColumns() const;
    ObjectType GetObjectType() const;         // TABLE or VIEW
    idx_t GetApproxRowCount() const;

private:
    vector<MSSQLColumnInfo> mssql_columns_;
    ObjectType object_type_;
    idx_t approx_row_count_;
};

} // namespace duckdb
```

---

## Error Handling

### Write Operation Rejection

All write-related methods throw `NotImplementedException`:

```cpp
// Pattern for rejecting writes
throw NotImplementedException("Write operations not supported for MSSQL catalog. "
                              "This extension is read-only.");
```

### Catalog Not Found

```cpp
// When schema doesn't exist
return nullptr;  // From LookupSchema

// When table doesn't exist
return nullptr;  // From LookupEntry
```

### Connection Errors

```cpp
// Propagate SQL Server errors with context
throw IOException("Failed to connect to SQL Server: %s", error_message);

// Connection timeout
throw IOException("SQL Server connection timeout after %d seconds", timeout_seconds);
```

---

## Thread Safety

- `MSSQLCatalog` is thread-safe via internal mutexes
- `MSSQLMetadataCache` uses mutex for all access
- `MSSQLSchemaSet` and `MSSQLTableSet` use atomic loading flags
- Connection pool from spec 003 provides thread-safe connection acquisition
