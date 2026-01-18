# Contract: DDL Translator

**Feature**: 008-catalog-ddl-statistics
**Module**: `mssql_ddl_translator`

## Purpose

Translates DuckDB catalog DDL operations into T-SQL statements for execution on SQL Server.

## Interface

```cpp
namespace duckdb {

//! Translates DuckDB DDL operations to T-SQL
class MSSQLDDLTranslator {
public:
    //! Quote a SQL Server identifier using bracket notation
    //! @param identifier The identifier to quote
    //! @return Quoted identifier with ] escaped as ]]
    static string QuoteIdentifier(const string &identifier);

    //! Generate CREATE SCHEMA T-SQL
    //! @param schema_name Schema to create
    //! @return T-SQL statement
    static string TranslateCreateSchema(const string &schema_name);

    //! Generate DROP SCHEMA T-SQL
    //! @param schema_name Schema to drop
    //! @return T-SQL statement
    static string TranslateDropSchema(const string &schema_name);

    //! Generate CREATE TABLE T-SQL from DuckDB BoundCreateTableInfo
    //! @param schema_name Target schema
    //! @param info DuckDB create table info with columns
    //! @return T-SQL statement
    static string TranslateCreateTable(const string &schema_name,
                                       BoundCreateTableInfo &info);

    //! Generate DROP TABLE T-SQL
    //! @param schema_name Schema containing table
    //! @param table_name Table to drop
    //! @return T-SQL statement
    static string TranslateDropTable(const string &schema_name,
                                     const string &table_name);

    //! Generate RENAME TABLE T-SQL via sp_rename
    //! @param schema_name Schema containing table
    //! @param old_name Current table name
    //! @param new_name New table name
    //! @return T-SQL statement
    static string TranslateRenameTable(const string &schema_name,
                                       const string &old_name,
                                       const string &new_name);

    //! Generate ADD COLUMN T-SQL
    //! @param schema_name Schema containing table
    //! @param table_name Table to modify
    //! @param column Column definition
    //! @return T-SQL statement
    static string TranslateAddColumn(const string &schema_name,
                                     const string &table_name,
                                     const ColumnDefinition &column);

    //! Generate DROP COLUMN T-SQL
    //! @param schema_name Schema containing table
    //! @param table_name Table to modify
    //! @param column_name Column to drop
    //! @return T-SQL statement
    static string TranslateDropColumn(const string &schema_name,
                                      const string &table_name,
                                      const string &column_name);

    //! Generate RENAME COLUMN T-SQL via sp_rename
    //! @param schema_name Schema containing table
    //! @param table_name Table containing column
    //! @param old_name Current column name
    //! @param new_name New column name
    //! @return T-SQL statement
    static string TranslateRenameColumn(const string &schema_name,
                                        const string &table_name,
                                        const string &old_name,
                                        const string &new_name);

    //! Generate ALTER COLUMN T-SQL for type change
    //! @param schema_name Schema containing table
    //! @param table_name Table containing column
    //! @param column_name Column to alter
    //! @param new_type New DuckDB type
    //! @param is_nullable Whether column allows nulls
    //! @return T-SQL statement
    static string TranslateAlterColumnType(const string &schema_name,
                                           const string &table_name,
                                           const string &column_name,
                                           const LogicalType &new_type,
                                           bool is_nullable);

    //! Generate ALTER COLUMN T-SQL for nullability change
    //! @param schema_name Schema containing table
    //! @param table_name Table containing column
    //! @param column_name Column to alter
    //! @param current_type Current column type
    //! @param set_not_null True to set NOT NULL, false for NULL
    //! @return T-SQL statement
    static string TranslateAlterColumnNullability(const string &schema_name,
                                                  const string &table_name,
                                                  const string &column_name,
                                                  const LogicalType &current_type,
                                                  bool set_not_null);

private:
    //! Map DuckDB LogicalType to SQL Server type string
    //! @param type DuckDB type
    //! @return SQL Server type string
    static string MapTypeToSQLServer(const LogicalType &type);
};

} // namespace duckdb
```

## Type Mapping

| DuckDB LogicalType | SQL Server Type |
| ------------------ | --------------- |
| BOOLEAN | BIT |
| TINYINT | TINYINT |
| SMALLINT | SMALLINT |
| INTEGER | INT |
| BIGINT | BIGINT |
| UTINYINT | TINYINT |
| USMALLINT | INT |
| UINTEGER | BIGINT |
| UBIGINT | DECIMAL(20,0) |
| FLOAT | REAL |
| DOUBLE | FLOAT |
| DECIMAL(p,s) | DECIMAL(p,s) |
| VARCHAR | NVARCHAR(n) |
| VARCHAR(MAX) | NVARCHAR(MAX) |
| BLOB | VARBINARY(MAX) |
| DATE | DATE |
| TIME | TIME(7) |
| TIMESTAMP | DATETIME2(6) |
| TIMESTAMP_TZ | DATETIMEOFFSET(7) |
| UUID | UNIQUEIDENTIFIER |

## Identifier Quoting

SQL Server identifiers are quoted with square brackets:
- `foo` → `[foo]`
- `foo]bar` → `[foo]]bar]`

## Error Conditions

- **Unsupported Type**: Throws `NotImplementedException` for unmapped DuckDB types
- **Invalid Identifier**: Throws `InvalidInputException` for identifiers > 128 characters

## Usage Example

```cpp
// Schema operations
string sql = MSSQLDDLTranslator::TranslateCreateSchema("new_schema");
// Result: CREATE SCHEMA [new_schema];

// Table operations
string sql = MSSQLDDLTranslator::TranslateDropTable("dbo", "old_table");
// Result: DROP TABLE [dbo].[old_table];

// Column operations
string sql = MSSQLDDLTranslator::TranslateRenameColumn("dbo", "users", "fname", "first_name");
// Result: EXEC sp_rename N'[dbo].[users].[fname]', N'first_name', N'COLUMN';
```
