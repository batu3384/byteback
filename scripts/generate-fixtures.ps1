# Golden fixture generator (placeholder).
# Real fixtures live in native/tests/fixtures/volume_fixtures.h as programmatic disks.
# Regenerate by editing buildFat16Volume / buildPngCarveDisk and running:
#   ctest --test-dir native/build -C Release -R GoldenRecovery

Write-Host "Golden fixtures are compiled in native/tests/fixtures/volume_fixtures.h"
Write-Host "Run: cmake --build native/build --config Release --target byteback_tests"
Write-Host "     ctest --test-dir native/build -C Release -R GoldenRecovery"
