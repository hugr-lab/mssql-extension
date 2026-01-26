# Quickstart: Code Cleanup and Directory Reorganization

## Verification Steps

This refactoring has no user-facing changes. Verification is done through build and test success.

### Step 1: Build After File Rename

```bash
# After renaming table_scan files
make clean && make

# Expected: Build succeeds with no errors
```

### Step 2: Run Tests After Each Change

```bash
# Run after every code removal or file move
make test

# Expected output:
# All tests passed (X skipped tests, 1371+ assertions in 56 test cases)
```

### Step 3: Verify No New Warnings

```bash
# Build and check for new warnings
make 2>&1 | grep -i warning

# Expected: No new warnings introduced
# (existing warnings may be present)
```

### Step 4: Verify Include Paths

```bash
# Build will fail if includes are wrong
make

# Can also verify with:
grep -r '#include.*insert/' src/ | head -5
grep -r '#include.*dml/insert/' src/ | head -5

# After refactoring, first should return nothing, second should show results
```

### Step 5: Verify Documentation

Check that these files reflect the new structure:
- `README.md` - Project Structure section
- `DEVELOPMENT.md` - Build instructions
- `docs/TESTING.md` - Test locations

## Rollback Procedure

If issues are found:

```bash
# Each change should be in a separate commit
git log --oneline -10  # Find the commit to revert

# Revert specific commit
git revert <commit-hash>

# Or reset to known good state
git reset --hard <good-commit>
```

## Success Criteria Checklist

- [ ] `make` succeeds without new errors
- [ ] `make test` passes all 1371+ assertions
- [ ] No new compiler warnings
- [ ] README.md shows updated directory structure
- [ ] DEVELOPMENT.md has accurate paths
- [ ] docs/TESTING.md has accurate paths
- [ ] Git history preserved via `git mv`
