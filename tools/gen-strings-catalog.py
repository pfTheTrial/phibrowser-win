#!/usr/bin/env python3
# Copyright 2026 The Fyde Innovations Limited. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Builds the Windows client's string catalog (resources/strings.json).

Translations are NOT authored here. The product's single source of
translations is the Mac catalog, Resources/Localizable.xcstrings, which the
dedicated translation repository maintains (see the Mac repo's
docs/i18n/localization-guidelines.md). This script joins the two:

  * `client/resources/strings-source.json` owns the KEYS the Windows client
    asks for, each with its English value and a translator comment. It is the
    Windows counterpart of the NSLocalizedString call sites on the Mac.

  * For every key, translations come from the Mac catalog — by key when the
    Mac carries that key, otherwise by exact English match, which is how the
    two clients share wording that was deliberately ported (a Windows "Pin"
    and a Mac "Pin" must never drift apart).

  * A Windows-only string with no Mac counterpart may carry its own
    `translations` block in the source file. Anything still untranslated is
    reported and falls back to English at runtime, which is the same
    degradation the Mac has for a new key awaiting the translation repo.

Usage:
    tools/gen-strings-catalog.py            # write resources/strings.json
    tools/gen-strings-catalog.py --check    # report only, touch nothing
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "client", "resources", "strings-source.json")
OUTPUT = os.path.join(REPO, "client", "resources", "strings.json")
MAC_CATALOG = os.path.join(
    os.path.dirname(os.path.dirname(REPO)), "Phi", "phibrowser-mac",
    "Resources", "Localizable.xcstrings")

# The locales the product ships. "en" leads: it is the source language and the
# runtime fallback.
LOCALES = ["en", "de", "es", "fr", "ja", "ko", "nl", "zh-Hans", "zh-Hant"]


def load_mac(path):
    """(by_key, by_english) views of the Mac catalog's translations."""
    if not os.path.exists(path):
        sys.exit(f"Mac catalog not found: {path}\n"
                 "It is the source of translations; clone phibrowser-mac beside "
                 "this repo or pass --mac-catalog.")
    with open(path, encoding="utf-8") as f:
        strings = json.load(f)["strings"]
    by_key, by_english = {}, {}
    for key, entry in strings.items():
        table = {}
        for locale, value in (entry.get("localizations") or {}).items():
            unit = value.get("stringUnit") or {}
            text = unit.get("value")
            # A "variations" entry is plural/device-specific; the Windows
            # client has no such call sites, so those keys are skipped rather
            # than flattened to an arbitrary branch.
            if text is not None:
                table[locale] = text
        if not table:
            continue
        by_key[key] = table
        english = table.get("en")
        # First writer wins: identical English under several keys means the
        # Mac gave them separate identities for context, and the Windows
        # source file names the key it wants explicitly in that case.
        if english and english not in by_english:
            by_english[english] = table
    return by_key, by_english


def normalize(text):
    """English as the Mac spells it: menus there carry no accelerators."""
    return text.split("\t")[0].replace("&", "").replace("…", "...").strip()


def build(source, by_key, by_english):
    out, report = {}, defaultdict(list)
    for key, entry in source["strings"].items():
        english = entry["en"]
        table = {"en": english}

        mac = by_key.get(key)
        origin = "mac-key"
        if mac is None:
            mac = by_english.get(normalize(english))
            origin = "mac-english"
        if mac is None:
            mac = by_english.get(normalize(english).replace("...", "…"))
            origin = "mac-english"

        # An explicit `translations` block is a deliberate Windows override and
        # WINS over the Mac match: it exists precisely where the shared wording
        # is wrong here — a menu title whose "&" mnemonic differs per language
        # ("&Datei", "\u6587\u4ef6(&F)"), which no amount of re-decoration can derive.
        # Such a value is taken VERBATIM.
        #
        # A Mac value, by contrast, is the translation of the BARE label: the
        # "&" mnemonic and the "\tCtrl+N" accelerator are Windows layout, not
        # language, so they are re-applied around it.
        own = entry.get("translations") or {}
        translations = own or mac or {}
        # Decoration: translations are authored and reused BARE, and the
        # Windows "&" mnemonic plus "\tCtrl+N" accelerator are re-applied
        # around them. An entry marked "verbatim" opts out because its
        # decoration is language-specific and cannot be derived — a menu
        # title picks its own mnemonic letter per language ("&Datei",
        # "\u6587\u4ef6(&F)"), so those values already carry it.
        if entry.get("verbatim"):
            prefix = accelerator = ""
        else:
            prefix = "&" if english.startswith("&") else ""
            accelerator = ("\t" + english.split("\t", 1)[1]) if "\t" in english else ""
        for locale, text in translations.items():
            if locale in LOCALES and locale != "en":
                table[locale] = prefix + text + accelerator
        report["own" if own else (origin if mac else "untranslated")].append(key)

        missing = [l for l in LOCALES if l not in table]
        if missing and key not in report["untranslated"]:
            report["partial"].append(key)
        out[key] = table
    return out, report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="report coverage without writing the catalog")
    parser.add_argument("--mac-catalog", default=MAC_CATALOG)
    args = parser.parse_args()

    with open(SOURCE, encoding="utf-8") as f:
        source = json.load(f)
    by_key, by_english = load_mac(args.mac_catalog)
    strings, report = build(source, by_key, by_english)

    total = len(strings)
    print(f"keys                    : {total}")
    print(f"  translated by Mac key : {len(report['mac-key'])}")
    print(f"  translated by English : {len(report['mac-english'])}")
    print(f"  translated in source  : {len(report['own'])}")
    print(f"  ENGLISH ONLY          : {len(report['untranslated'])}")
    if report["untranslated"]:
        print("\nawaiting translation (falls back to English at runtime):")
        for key in sorted(report["untranslated"])[:40]:
            print("   ", key)
        if len(report["untranslated"]) > 40:
            print(f"    … and {len(report['untranslated']) - 40} more")

    if args.check:
        return 0

    payload = {"locales": LOCALES, "strings": dict(sorted(strings.items()))}
    with open(OUTPUT, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    print(f"\nwrote {OUTPUT} ({os.path.getsize(OUTPUT):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
