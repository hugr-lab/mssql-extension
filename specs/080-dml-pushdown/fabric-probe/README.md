# Fabric probe for spec 080 (D0)

Each file answers one question from the Fabric column of
`../recon-2026-10-08.md` § 4, against a live Fabric Warehouse:

| file | what it checks |
|---|---|
| `p00_setup` | creates the probe tables |
| `p01_output` | OUTPUT |
| `p02_output_into_tvar` | OUTPUT INTO a table variable |
| `p03_rowcount` | `@@ROWCOUNT` |
| `p04_out_param` | an OUTPUT parameter through `sp_executesql` |
| `p05_update_from_join` | UPDATE … FROM … JOIN, aliased target |
| `p06_update_from_join_bare` | UPDATE … FROM … JOIN, bare target |
| `p07_delete_from_join` | DELETE … FROM … JOIN |
| `p08_update_exists` | the `WHERE EXISTS` form of UPDATE |
| `p09_delete_exists` | the `WHERE EXISTS` form of DELETE |
| `p10_merge` | MERGE with UPDATE / DELETE / INSERT actions |
| `p11_merge_count` | the count a MERGE reports |
| `p12_merge_output` | MERGE … OUTPUT |
| `p13_is_distinct` | `IS NOT DISTINCT FROM` |
| `p14_intersect_form` | the `EXISTS (… INTERSECT …)` null-safe form |
| `p15_bulk_into_temp` | `INSERT BULK` into `#stage` inside a transaction |
| `p16_dml_count_done` | the DONE count of a plain UPDATE |
| `p17_server_properties` | `EngineEdition`, `ProductMajorVersion`, `@@VERSION`, `DB_NAME()` |
| `p99_cleanup` | drops the probe tables |

**The files are probes, not tests.** The ones that ask a question expect a
placeholder `X`, so a run prints the server's actual answer or its error
under "Actual result". Run them one at a time, in order, with
`AZURE_WH_HOST`, `AZURE_WH_NAME` and `AZURE_WH_TOKEN` set:

```bash
export AZURE_WH_TOKEN=$(az account get-access-token --resource https://database.windows.net/ --query accessToken -o tsv)
mkdir -p test/sql/zz_fabric_probe && cp specs/080-dml-pushdown/fabric-probe/p*.test test/sql/zz_fabric_probe/
for f in test/sql/zz_fabric_probe/p*.test; do ./build/release/test/unittest "$f"; done
rm -rf test/sql/zz_fabric_probe
```

Never paste the token into an issue or a log: the runner echoes the failing
ATTACH, token included.

**Status:**
- 2026-10-08: not run. The warehouse refused every login with 18456
  "Couldn't complete the operation due to a system update". D0's Fabric rows
  follow the documentation until these are run.
- Once they are, record the answers here and flip the D0 rows they settle.
