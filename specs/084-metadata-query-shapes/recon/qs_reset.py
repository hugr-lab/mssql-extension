import sys
print("ATTACH '${DSN}' AS adm (TYPE mssql);\nSELECT mssql_exec('adm', 'ALTER DATABASE "+sys.argv[1]+" SET QUERY_STORE CLEAR;');")
