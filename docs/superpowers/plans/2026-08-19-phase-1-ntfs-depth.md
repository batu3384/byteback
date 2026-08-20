# Phase 1 — NTFS Depth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans.

**Goal:** Professional NTFS forensic depth: scored deleted MFT records, structural $LogFile RCRD hints, USN counts in scan summary/report.

**Spec:** `docs/superpowers/specs/2026-08-19-byteback-professional-roadmap-design.md` (Phase 1)

## Delivered

- [x] `scoreMftConfidence()` — deleted records with data runs score higher
- [x] `$LogFile` v2 — RCRD client-data scan + non-resident $DATA read
- [x] `ScanSummary` USN fields — timelineEvents, usnCreates/Deletes/Renames
- [x] Report HTML USN row
- [x] Tests: `test_ntfs_logfile.cpp`, `test_usn_summary.cpp`

## Next (Phase 1b)

- [ ] Cross-boost MFT confidence when USN DELETE matches MFT ref
- [ ] `$LogFile` redo opcode parsing (OpenNonresidentAttribute)
- [ ] Dedicated USN export CSV from TimelineView
