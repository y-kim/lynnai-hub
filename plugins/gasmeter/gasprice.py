#!/usr/bin/env python3
"""Fetch the current city gas price from the gas supplier and write it for gasmeter.py --price.

    gasprice.py --list                              suppliers and the regions each one knows
    gasprice.py --supplier koone --region 성남      fetch, write ~/gas/price.json, remember the choice
    gasprice.py                                     fetch again for the remembered supplier / region (cron)
    gasprice.py --show                              print only

Each supplier is a plugin in gasprice_suppliers/<name>.py (see gasprice_suppliers/__init__.py for what
a plugin provides). Korean city gas is billed by heat, so the plugin returns the four published numbers
and this program works out the price per m³ the meter counts:

    won per m³ (before VAT) = factor × heat × rate
      rate    won per MJ, residential (published monthly)
      heat    average heat value of the gas, MJ per m³ (published per month)
      factor  temperature / pressure correction for the region (published now and then)
    base      monthly base charge, won

The supplier and region are kept in ~/gas/gasprice.json (--config) after the first run that names them.
Run it once a day from cron; gasmeter.py reads the file again on every send. On failure the old file
is kept, so the last good price stays in use.
"""
import argparse
import json
import os
import sys
from datetime import date

from i18n import _
import gasprice_suppliers


def write(path, text):
    """Replace the file whole, so a power cut leaves the old or the new one, never half of it."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text + "\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--supplier", help=_("Supplier plugin (see --list)"))
    p.add_argument("--region", help=_("Region, as the supplier names it (see --list)"))
    p.add_argument("--out", default=os.path.expanduser("~/gas/price.json"), help=_("File to write"))
    p.add_argument("--config", default=os.path.expanduser("~/gas/gasprice.json"),
                   help=_("File that remembers the supplier and region"))
    p.add_argument("--show", action="store_true", help=_("Print the result instead of writing it"))
    p.add_argument("--list", action="store_true", help=_("List the suppliers and their regions"))
    a = p.parse_args()

    if a.list:
        for name, mod in gasprice_suppliers.all_plugins().items():
            print(f"{name:10s} {mod.TITLE}: {', '.join(mod.REGIONS)}")
        return
    saved = {}
    try:
        with open(a.config, encoding="utf-8") as f:
            saved = json.load(f)
    except (OSError, ValueError):
        pass
    a.supplier = a.supplier or saved.get("supplier")
    a.region = a.region or saved.get("region")
    if not a.supplier or not a.region:
        p.error(_("--supplier and --region are needed the first time (see --list)"))
    mod = gasprice_suppliers.load(a.supplier)
    if a.region not in mod.REGIONS:
        sys.exit(_("{supplier} has no region {region}; it has: {regions}",
                   supplier=a.supplier, region=a.region, regions=", ".join(mod.REGIONS)))
    if (saved.get("supplier"), saved.get("region")) != (a.supplier, a.region) and not a.show:
        write(a.config, json.dumps({"supplier": a.supplier, "region": a.region}, ensure_ascii=False, indent=1))

    try:
        got = mod.fetch(a.region)
    except gasprice_suppliers.SupplierError as e:
        sys.exit(_("{supplier}: {error} (kept the old price file)", supplier=a.supplier, error=e))
    price = {"supplier": a.supplier, "region": a.region, **got}
    price.setdefault("vat", 0.1)
    price["won_per_m3"] = round(price["factor"] * price["heat"] * price["rate"], 4)
    price["fetched"] = date.today().isoformat()

    text = json.dumps(price, ensure_ascii=False, indent=1)
    if a.show:
        print(text)
        return
    write(a.out, text)
    print(_("{date} {supplier}/{region}: {won:.2f} won/m³ before VAT, base {base} won/month",
            date=price["fetched"], supplier=a.supplier, region=a.region,
            won=price["won_per_m3"], base=price["base"]))


if __name__ == "__main__":
    main()
