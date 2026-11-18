"""코원에너지서비스 (Seoul / Gyeonggi city gas, www.skens.com/koone).

Everything comes from the supplier's public pages:
  rate, base  자료실 (data/list.do): newest "요금단가 변경 안내" post; its attached .xlsx has one table per
              tariff area (서울 / 경기). Row 주택용 취사 (same rate as 주택용 난방 up to 516 MJ), column F
              = the new rate; column I says "기본요금 1,250원/월".
  factor      자료실: newest "온압보정계수" post, lines like "성남/과천/하남: 0.9943 → 0.9939".
  heat        열량계수조회 (caloric/call_EBPP_044.do, company code B000): average for this month so far,
              or last month's at the very start of a month.
"""
import html
import io
import json
import re
import urllib.parse
import urllib.request
import zipfile
from datetime import date, timedelta

from i18n import _
from . import SupplierError

TITLE = "코원에너지서비스"
SITE = "https://www.skens.com/koone/"
# region -> tariff area in the rate table (the factor notice names the regions themselves)
AREA = {"서울": "서울", "성남": "경기", "과천": "경기", "하남": "경기", "광주": "경기", "이천": "경기", "여주": "경기"}
REGIONS = list(AREA)
COMPANY = "B000"            # 열량계수조회 company code


def _get(url, data=None, referer=None):
    headers = {"User-Agent": "Mozilla/5.0"}
    if referer:
        headers["Referer"] = referer
    body = urllib.parse.urlencode(data).encode() if data is not None else None
    with urllib.request.urlopen(urllib.request.Request(url, body, headers), timeout=30) as r:
        return r.read()


def _posts():
    """(seq, title) of the 자료실 list, newest first."""
    page = _get(SITE + "data/list.do").decode("utf-8", "replace")
    return [(int(s), html.unescape(t).strip())
            for s, t in re.findall(r'view\.do\?form\.seq=(\d+)[^"]*">([^<]+)</a>', page)]


def _newest(posts, words):
    for seq, title in posts:
        if words in title:
            return seq, title
    raise SupplierError(_("No post titled \"{words}\" on the supplier's list", words=words))


def _view(seq):
    return _get(SITE + f"data/view.do?form.seq={seq}").decode("utf-8", "replace")


def _xlsx_rows(blob):
    """Rows of the first sheet as {column letter: text}."""
    z = zipfile.ZipFile(io.BytesIO(blob))
    shared = []
    if "xl/sharedStrings.xml" in z.namelist():
        x = z.read("xl/sharedStrings.xml").decode()
        shared = ["".join(re.findall(r"<t[^>]*>(.*?)</t>", si, re.S))
                  for si in re.findall(r"<si>(.*?)</si>", x, re.S)]
    x = z.read("xl/worksheets/sheet1.xml").decode()
    for row in re.findall(r"<row[^>]*>(.*?)</row>", x, re.S):
        cells = {}
        for col, attrs, inner in re.findall(r'<c r="([A-Z]+)\d+"([^>]*?)(?:/>|>(.*?)</c>)', row, re.S):
            v = re.search(r"<v>(.*?)</v>", inner or "")
            if v:
                cells[col] = shared[int(v.group(1))] if 't="s"' in attrs else v.group(1)
        yield cells


def _tariff(posts, area):
    seq, title = _newest(posts, "요금단가 변경 안내")
    page = _view(seq)
    files = re.findall(r'name="seq" value="([^"]+)">\s*<a[^>]*>([^<]+)</a>', page)
    xlsx = [s for s, name in files if name.strip().lower().endswith(".xlsx")]
    if not xlsx:
        raise SupplierError(_("\"{title}\" has no .xlsx attached", title=title))
    blob = _get(SITE + "data/download.do", {"seq": xlsx[0]}, SITE + f"data/view.do?form.seq={seq}")
    in_area, rate, base = False, None, None
    for c in _xlsx_rows(blob):
        a = c.get("A", "")
        if "도시가스요금" in a:                         # a table title: "경기-도시가스요금 조정 내역 (...)"
            in_area = a.startswith(area)
        label = c.get("C", "").replace(" ", "")
        if in_area and label in ("주택용취사", "주택용(취사,난방)"):
            rate = float(c["F"])
            m = re.search(r"기본요금\s*([\d,]+)", c.get("I", ""))
            base = int(m.group(1).replace(",", "")) if m else None
    if rate is None:
        raise SupplierError(_("No residential rate for {area} in \"{title}\"", area=area, title=title))
    return rate, base if base is not None else 1250, title


def _factor(posts, region):
    seq, title = _newest(posts, "온압보정계수")
    text = html.unescape(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", _view(seq))))
    m = re.search(region + r"[^0-9]{0,40}?(0\.\d+)\s*(?:→|->|⇒)\s*(0\.\d+)", text)
    if m:
        return float(m.group(2)), title
    m = re.search(region + r"[^0-9]{0,40}?(0\.\d+)", text)
    if m:
        return float(m.group(1)), title
    raise SupplierError(_("No correction factor for {region} in \"{title}\"", region=region, title=title))


def _heat():
    ref = SITE + "caloric/caloricInq.do"
    first = date.today().replace(day=1)
    last_month_end = first - timedelta(days=1)
    for a, b in ((first, date.today()), (last_month_end.replace(day=1), last_month_end)):
        r = json.loads(_get(SITE + "caloric/call_EBPP_044.do",
                            {"I_FDATE": f"{a:%Y%m%d}", "I_TDATE": f"{b:%Y%m%d}", "I_CALOR": COMPANY}, ref))
        rows = r.get("list") or []
        if rows and float(rows[0].get("E_CALOR") or 0) > 0:
            return float(rows[0]["E_CALOR"]), f"{a:%Y-%m-%d}..{b:%Y-%m-%d}"
    raise SupplierError(_("No average heat value for this or last month"))


def fetch(region):
    posts = _posts()
    rate, base, rate_post = _tariff(posts, AREA[region])
    factor, factor_post = _factor(posts, region)
    heat, heat_span = _heat()
    return {"rate": rate, "base": base, "factor": factor, "heat": heat, "vat": 0.1,
            "sources": {"rate": rate_post, "factor": factor_post, "heat": heat_span}}
