"""Gas supplier plugins for gasprice.py: one module per supplier, named after it (koone.py, ...).

A plugin module provides:
    TITLE     the supplier's name, for --list
    REGIONS   the region names it accepts (as the supplier writes them)
    fetch(region) -> dict with
        rate      won per MJ, residential, before VAT
        base      monthly base charge, won, before VAT
        factor    temperature / pressure correction factor for the region
        heat      average heat value, MJ per m³
        vat       (optional, default 0.1)
        sources   (optional) dict of where each number came from, kept in the output for checking

Plugins raise SupplierError with a readable message when the supplier's pages are not as expected.
"""
import importlib
import pkgutil

from i18n import _


class SupplierError(Exception):
    pass


def names():
    return sorted(m.name for m in pkgutil.iter_modules(__path__) if not m.name.startswith("_"))


def load(name):
    if name not in names():
        raise SystemExit(_("Unknown supplier {name}; known: {known}", name=name, known=", ".join(names())))
    return importlib.import_module(f"{__name__}.{name}")


def all_plugins():
    return {n: load(n) for n in names()}
