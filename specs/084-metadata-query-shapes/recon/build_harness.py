import re, sys, json
import os
ROOT=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'src', 'catalog') + os.sep
def const(path, name):
    s=open(ROOT+path).read()
    m=re.search(r'static const char \*'+name+r' = R"\((.*?)\)";', s, re.S)
    return m.group(1).strip() if m else ''  # gone from the source: queries.json keeps the measured text
def coll():
    s=open(ROOT+'mssql_catalog.cpp').read()
    m=re.search(r'DATABASE_COLLATION_SQL =\s*(.*?);\n', s, re.S)
    return "".join(re.findall(r'"(.*?)"', m.group(1)))
Q={
 'schema_disc':   (const('mssql_metadata_cache.cpp','SCHEMA_DISCOVERY_SQL')+"\nORDER BY s.name", ''),
 'table_disc':    (const('mssql_metadata_cache.cpp','TABLE_DISCOVERY_SQL_TEMPLATE')+"\nORDER BY o.name", '@s sysname'),
 'table_names':   (const('mssql_metadata_cache.cpp','TABLE_NAMES_SQL'), ''),
 'single_table':  (const('mssql_metadata_cache.cpp','SINGLE_TABLE_METADATA_SQL_TEMPLATE'), '@s sysname, @t sysname'),
 'pk_disc':       (const('mssql_primary_key.cpp','PK_DISCOVERY_SQL_TEMPLATE'), '@s sysname, @t sysname'),
 'column_disc':   (const('mssql_metadata_cache.cpp','COLUMN_DISCOVERY_SQL_TEMPLATE'), '@s sysname, @t sysname'),
 'row_count':     (const('mssql_statistics.cpp','ROW_COUNT_SQL_TEMPLATE'), '@s sysname, @t sysname'),
 'bulk_schema':   (const('mssql_metadata_cache.cpp','BULK_METADATA_SCHEMA_SQL_TEMPLATE')+"\nORDER BY s.name, o.name, c.column_id", '@s sysname'),
 'bulk_all':      (const('mssql_metadata_cache.cpp','BULK_METADATA_ALL_SQL'), ''),
 'db_collation':  (coll(), ''),
}
import os
if os.path.exists('queries.json'):
    Q=json.load(open('queries.json'))  # QJSON
else:
    json.dump(Q, open('queries.json','w'), indent=1)
def mark(sql, tag):
    return re.sub(r'SELECT', 'SELECT /*mdq:'+tag+'*/', sql, count=1)
def variant(sql, v):
    sql=sql.rstrip().rstrip(';')
    if v=='v0': return sql
    if v=='maxdop1': return sql+"\nOPTION (MAXDOP 1)"
    if v=='keepfixed': return sql+"\nOPTION (KEEPFIXED PLAN)"
def tsql_lit(s): return "N'"+s.replace("'","''")+"'"
def harness(db, s, t, names, variants, cold=5, warm=10):
    out=[f"USE {db};", "SET NOCOUNT ON;", "DECLARE @stmt nvarchar(max), @p nvarchar(200), @k int;"]
    for n in names:
        sql, params = Q[n]
        sql = sql.replace('__S__', s).replace('__T__', t)
        for v in variants:
            stmt=mark(variant(sql, v), f"{n}:{v}")
            out.append(f"SET @stmt = {tsql_lit(stmt)}; SET @p = {tsql_lit(params)};")
            call = "EXEC sp_executesql @stmt" + (", @p" if params else "") + (f", @s = N'{s}'" if '@s' in params else "") + (f", @t = N'{t}'" if '@t' in params else "") + (f", @o = N'[{s}].[{t}]'" if '@o' in params else "") + ";"
            out.append(f"SET @k = 0; WHILE @k < {cold} BEGIN ALTER DATABASE SCOPED CONFIGURATION CLEAR PROCEDURE_CACHE; {call} SET @k += 1; END;")
            out.append(f"SET @k = 0; WHILE @k < {warm} BEGIN {call} SET @k += 1; END;")
    return "\n".join(out)
if __name__=='__main__':
    db, s, t, names, variants = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4].split(','), sys.argv[5].split(',')
    cold=int(sys.argv[6]) if len(sys.argv)>6 else 5; warm=int(sys.argv[7]) if len(sys.argv)>7 else 10
    h=harness(db,s,t,names,variants,cold,warm)
    print("SET mssql_query_timeout = 0;\nATTACH '${DSN}' AS adm (TYPE mssql);\nSELECT mssql_exec('adm', "+"'"+h.replace("'","''")+"');")
