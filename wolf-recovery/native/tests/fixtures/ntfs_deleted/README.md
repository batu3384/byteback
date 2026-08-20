# NTFS deleted golden fixtures

Programmatic equivalents live in `volume_fixtures.h`:

- `buildNtfsDeletedResidentVolume()` — deleted MFT record with resident `doc.txt` / `hello`
- Wired by `test_recovery_golden.cpp` (`GoldenRecovery.NtfsDeletedResidentFindAndRecover`)

Optional future: add binary `.img` snapshots here for cross-tool regression.
