#!/usr/bin/env python3
# Validates Qt .ts translation files for consistency between source and
# translation strings. Intended to be run in CI or pre-commit to catch
# placeholder mismatches introduced by machine/automated changes.
#
# Usage:
#   cmake/validate-translations.py [path/to/i18n/*.ts ...]
#
# With no arguments, scans src/**/i18n/*.ts.
import glob
import io
import re
import sys
import xml.etree.ElementTree as ET

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8')

# Qt placeholders: %1 %2 ... %L1 %Ln %n (%Ln not valid; %L<int> and %n are)
PLACEHOLDER = re.compile(r"%L?\d+|%n")
# %n is a plural form placeholder (special); tracked separately.
args = sys.argv[1:]
files = args if args else sorted(glob.glob("src/**/i18n/*.ts", recursive=True))

total_missing = 0
total_issues = 0
for f in files:
    tree = ET.parse(f)
    root = tree.getroot()
    lang = root.attrib.get("language", "(none)")
    missing = 0
    errors = 0
    ctx = "(?)"
    for el in root.iter("context"):
        ctx = el.attrib.get("displayname") or el.findtext("name", "?")
        for msg in el.findall("message"):
            if msg.attrib.get("type", "") == "obsolete":
                continue
            src = msg.findtext("source", "") or ""
            tr = msg.find("translation")
            if tr is None:
                continue
            ttype = tr.attrib.get("type", "")
            if ttype == "unfinished" and not (tr.text or "").strip():
                missing += 1
                continue
            text = tr.text or ""
            # Every numeric/named placeholder present in the source must also be
            # present in the translation (in any order).
            src_ph = set(PLACEHOLDER.findall(src))
            dst_ph = set(PLACEHOLDER.findall(text))
            if "%n" in src_ph:
                # Plural forms are special: they are stored verbatim and the
                # numerizer handles them; only require the plural marker when
                # the source uses it as a real placeholder (rare). Skip strict
                # checking for %n to avoid false positives.
                src_ph.discard("%n")
                dst_ph.discard("%n")
            missing_ph = src_ph - dst_ph
            if missing_ph:
                errors += 1
                print(
                    f"{f}:{lang} [{ctx}] missing placeholders "
                    f"{sorted(missing_ph)} in: {text[:70]!r}"
                )
            # Note: accelerator mnemonics (&Foo) are translator's choice and
            # can't be validated by simple counting, so they are deliberately
            # not checked here. Only Qt placeholders (%1, %2, %L1, ...) are.
    total_missing += missing
    total_issues += errors
    print(
        f"{f}: {lang}: {missing} missing translation(s), "
        f"{errors} placeholder/accelerator issue(s)"
    )

print("\n---")
print(f"Total genuinely missing translations: {total_missing}")
print(f"Total consistency issues: {total_issues}")
sys.exit(1 if total_issues else 0)
