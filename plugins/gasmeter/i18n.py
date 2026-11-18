"""Tiny message translation helper (identical copy in capture/, hw/, vendors/*/cloud/).

Code writes English: _("Saved {n} files", n=3). Translations live next to this
file in locale/<lang>.json as {"English source": "translation"}. Missing
entries fall back to English. Language: BOILERCTL_LANG, else LC_ALL /
LC_MESSAGES / LANG (e.g. ko_KR.UTF-8 -> ko), else English.
Each folder keeps its own copy so a single folder (e.g. capture/ on a Pi)
runs on its own; tests/check_i18n.py keeps the copies identical.
"""
import json
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_catalog = None


def lang():
    for var in ("BOILERCTL_LANG", "LC_ALL", "LC_MESSAGES", "LANG"):
        v = os.environ.get(var, "")
        if v:
            code = v.split(".")[0].split("_")[0].lower()
            return "en" if code in ("c", "posix") else code
    return "en"


def _load():
    global _catalog
    if _catalog is None:
        _catalog = {}
        path = os.path.join(_HERE, "locale", lang() + ".json")
        try:
            with open(path, encoding="utf-8") as f:
                _catalog = json.load(f)
        except (OSError, ValueError):
            pass
    return _catalog


def _(msg, **kw):
    """Translate msg, then fill {placeholders} from kw."""
    text = _load().get(msg, msg)
    return text.format(**kw) if kw else text
