"""
devices.py -- 린나이 기기 분류체계 + 식별자 통일 + 기기간 연계.

목표:
- boiler 전용(main/zone)에서 벗어나 여러 기기종류(boiler, thermostat, steam_oven,
  cooktop, induction, sensor, ...)를 한 모델로 담는다.
- 식별자는 전부 config(lynnai)에서 파생: unique_id = <prefix>_<class>_<instance>,
  토픽 = <base>/<class>/<instance>/<attr>.
- 기기 연계: thermostat 는 특정 boiler 의 zone 에 묶인다(boiler↔thermostat).
  HA/ST 에선 via_device 로 묶여 보인다.

새 기기 추가 = CLASS_SPECS 에 한 줄 + TYPE_MAP(린나이 f코드→class) 한 줄.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "config"))
import defaults as cfgmod  # noqa: E402


# ── 기기 종류 명세 ───────────────────────────────────────────────
# ha      : Home Assistant/ST 컴포넌트(climate, water_heater, switch, sensor, ...)
# attrs   : 상태/명령 속성 (command=True 면 .../set 받음)
# links_to: 이 종류가 어떤 부모에 연계되는지(없으면 None). thermostat→boiler.
CLASS_SPECS = {
    "boiler": {
        "label": "Boiler",
        "ha": "water_heater",
        "attrs": {
            "power": {"command": True, "type": "bool"},
            "heat": {"command": True, "type": "bool"},
            "hotwater": {"command": True, "type": "bool"},
            "heat_temp": {"command": True, "type": "number"},
            "hw_temp": {"command": True, "type": "number"},
            "heat_mode": {"command": True, "type": "enum"},   # 온돌/실내
            "status": {"command": False, "type": "avail"},
            # 안전 알람(읽기, HA binary_sensor). 상태 70블롭에서 디코드.
            "frozen": {"command": False, "type": "alarm", "device_class": "cold"},    # 동파 경보 (위치28·29)
            "error": {"command": False, "type": "alarm", "device_class": "problem"},  # 에러 (위치21, 코드 23-24)
        },
        "links_to": None,
    },
    "thermostat": {           # 방 온도조절기. boiler 의 zone 이거나 단독(0 또는 1 boiler)
        "label": "Thermostat",
        "ha": "climate",
        "attrs": {
            "mode": {"command": True, "type": "enum"},        # heat/off
            "target": {"command": True, "type": "number"},
            "current": {"command": False, "type": "number"},
        },
        "links_to": "boiler",   # boiler 에 '연계 가능'(선택). 링크 없으면 단독 동작.
    },
    "steam_oven": {
        "label": "Steam Oven",
        "ha": "climate",
        "attrs": {
            "power": {"command": True, "type": "bool"},
            "mode": {"command": True, "type": "enum"},
            "target": {"command": True, "type": "number"},
            "timer": {"command": True, "type": "number"},
            "status": {"command": False, "type": "avail"},
        },
        "links_to": None,
    },
    "cooktop": {              # 가스쿡탑 (버너 N개)
        "label": "Gas Cooktop",
        "ha": "sensor",       # 대개 읽기/안전표시(원격점화는 규제/안전상 보통 불가)
        "attrs": {
            "burner": {"command": False, "type": "level", "indexed": True},
            "status": {"command": False, "type": "avail"},
        },
        "links_to": None,
    },
    "induction": {           # 인덕션 레인지 (구역 N개)
        "label": "Induction Range",
        "ha": "sensor",
        "attrs": {
            "zone": {"command": False, "type": "level", "indexed": True},
            "status": {"command": False, "type": "avail"},
        },
        "links_to": None,
    },
    "sensor": {              # 온습도계 등
        "label": "Sensor",
        "ha": "sensor",
        "attrs": {
            "temperature": {"command": False, "type": "number"},
            "humidity": {"command": False, "type": "number"},
        },
        "links_to": None,
    },
}

# 린나이 프로토콜 타입코드(f06, f09, ...) → 우리 class.
# 새 기기 f코드 파악되면 여기 추가(스팀오븐/쿡탑/인덕션 코드 TBD).
TYPE_MAP = {
    "f06": "boiler",
    "f09": "thermostat",     # 각방 조절기
}


def mac_suffix(mac):
    """MAC → 인스턴스용 짧은 식별자. '34:5F:45:00:00:02' → '000002'."""
    parts = [p for p in mac.replace("-", ":").split(":") if p]
    return "".join(parts[-3:]).lower()


class Device:
    def __init__(self, cls, instance, name=None, links=None, cfg=None, caps=None,
                 virtual=False):
        if cls not in CLASS_SPECS:
            raise ValueError("unknown device class: %s" % cls)
        self.cls = cls
        self.instance = instance              # 기기 인스턴스 식별(예: mac suffix, zone 포함)
        self.name = name or "%s %s" % (cls, instance)
        self.links = links or {}              # 예: {"boiler": "<instance>", "zone": 1}
        self.cfg = cfg or cfgmod.load()
        self.caps = caps                      # 선택: 버너/구역 개수 등
        self.virtual = virtual                # 실제 기기 없이 합성한 대표 기기(가상 boiler 등)

    @property
    def spec(self):
        return CLASS_SPECS[self.cls]

    def unique_id(self):
        return "%s_%s_%s" % (self.cfg["device_id_prefix"], self.cls, self.instance)

    def topic(self, *attr):
        # <base>/<class>/<instance>[/<attr>...]
        return "/".join([self.cfg["topic_base"], self.cls, self.instance, *map(str, attr)])

    def parent_uid(self):
        """연계 부모의 unique_id (thermostat→boiler). 없으면 None."""
        pcls = self.spec["links_to"]
        if not pcls:
            return None
        pinst = self.links.get(pcls)
        if not pinst:
            return None
        return "%s_%s_%s" % (self.cfg["device_id_prefix"], pcls, pinst)

    def ha_device_block(self):
        """HA/ST 디스커버리의 device 블록. via_device 로 부모에 연계."""
        blk = {
            "identifiers": [self.unique_id()],
            "name": self.name,
            "manufacturer": self.cfg["manufacturer"],
            "model": "%s %s" % (self.cfg["brand"], self.spec.get("label", self.cls)),
        }
        p = self.parent_uid()
        if p:
            blk["via_device"] = p             # ← boiler↔thermostat 연계가 여기서 드러남
        return blk

    def command_attrs(self):
        return [a for a, m in self.spec["attrs"].items() if m.get("command")]

    def __repr__(self):
        link = (" -> %s" % self.parent_uid()) if self.parent_uid() else ""
        v = " (virtual)" if self.virtual else ""
        return "<Device %s%s%s>" % (self.unique_id(), v, link)


class Registry:
    """등록된 기기들. f코드+MAC로 만들거나 직접 추가. 연계 질의 지원."""
    def __init__(self, cfg=None):
        self.cfg = cfg or cfgmod.load()
        self.devices = {}                     # unique_id -> Device

    def add(self, dev):
        self.devices[dev.unique_id()] = dev
        return dev

    def from_rinnai(self, type_code, mac, name=None, **kw):
        """린나이 f코드+MAC로 기기 생성(boiler, 센서 등 단일 인스턴스)."""
        cls = TYPE_MAP.get(type_code)
        if not cls:
            raise ValueError("미지원 린나이 타입코드: %s" % type_code)
        inst = mac_suffix(mac)
        return self.add(Device(cls, inst, name=name, cfg=self.cfg, **kw))

    def add_thermostat_zone(self, boiler, zone, module_mac=None, name=None):
        """boiler 의 한 난방구역 = thermostat 1개. 한 boiler 에 여러 개 가능.
        식별자는 boiler-scoped(lynnai_thermostat_<boiler>_z<zone>) — f09 하드웨어가
        바뀌어도 방(구역) 식별이 유지된다. module_mac 은 '명령 전송 하드웨어'로
        links 에만 보관(식별자 아님). 서로 다른 f09 모듈이 같은 boiler 의 다른
        구역을 맡아도 모두 이 boiler 아래로 묶인다."""
        inst = "%s_z%s" % (boiler.instance, zone)
        links = {"boiler": boiler.instance, "zone": zone}
        if module_mac:
            links["module"] = mac_suffix(module_mac)   # 전송 하드웨어(f09), 식별자 아님
        return self.add(Device("thermostat", inst, name=name,
                               links=links, cfg=self.cfg))

    def add_virtual_boiler(self, module_mac, name=None):
        """실제 f06 boiler 기기가 없을 때, thermostat 이 제어하는 '보일러'를 대표하는
        가상 boiler. thermostat 에서 보일러 온도·온수(heat_temp/hw_temp)·전원·모드를
        설정할 수 있으므로, 이를 HA/ST 에 boiler 기기로 노출한다. 보일러 레벨 명령은
        links['module'](이 f09)로 라우팅. key(f09 MAC)로 식별, 멱등."""
        inst = "v%s" % mac_suffix(module_mac)
        uid = "%s_boiler_%s" % (self.cfg["device_id_prefix"], inst)
        if uid in self.devices:
            return self.devices[uid]
        dev = Device("boiler", inst, name=name or "보일러", cfg=self.cfg,
                     links={"module": mac_suffix(module_mac)}, virtual=True)
        return self.add(dev)

    def add_standalone_thermostat(self, mac, zone=1, name=None):
        """실제 boiler 가 없는 thermostat. thermostat 도 보일러를 제어하므로 '진짜 단독'이
        아니라 — 가상 boiler 를 만들어 연결한다. 같은 모듈(mac)의 여러 zone 은 같은
        가상 boiler 아래로 묶이고, 그 가상 boiler 로 보일러 온도·온수 제어가 노출된다."""
        vb = self.add_virtual_boiler(mac)
        return self.add_thermostat_zone(vb, zone, module_mac=mac, name=name)

    def children_of(self, boiler):
        """이 boiler 에 연계된 thermostat 들(단독 thermostat 은 제외)."""
        return [d for d in self.devices.values()
                if d.parent_uid() == boiler.unique_id()]

    def standalone(self, cls=None):
        """부모 링크가 없는 기기들(단독 thermostat, boiler, 오븐 등). cls 로 필터."""
        return [d for d in self.devices.values()
                if d.parent_uid() is None and (cls is None or d.cls == cls)]

    def by_class(self, cls):
        return [d for d in self.devices.values() if d.cls == cls]


if __name__ == "__main__":
    reg = Registry()
    # 하나의 boiler + 여러 thermostat(N:1). f09 모듈 1개가 여러 구역을 맡는 예.
    boiler = reg.from_rinnai("f06", "34:5F:45:00:00:01", name="보일러")
    f09 = "34:5F:45:00:00:02"
    t1 = reg.add_thermostat_zone(boiler, 1, module_mac=f09, name="방1")
    t2 = reg.add_thermostat_zone(boiler, 2, module_mac=f09, name="방2")
    t6 = reg.add_thermostat_zone(boiler, 6, module_mac=f09, name="거실")
    # 실제 f06 없는 thermostat: 가상 boiler 가 자동 생성되어 연결됨
    solo = reg.add_standalone_thermostat("11:22:33:44:55:66", zone=1, name="단독 온도조절기")
    vboiler = reg.devices[solo.parent_uid()]
    # 미래 기기: 스팀오븐(가상 MAC)
    oven = reg.add(Device("steam_oven", mac_suffix("AA:BB:CC:11:22:33"), name="스팀오븐"))

    print("== 등록된 기기 ==")
    for d in reg.devices.values():
        print(" ", d, "| topic base:", d.topic())
    print("\n== 한 boiler 에 묶인 thermostat 들 (N:1) ==")
    kids = reg.children_of(boiler)
    print("  boiler %s 의 thermostat %d개:" % (boiler.unique_id(), len(kids)))
    for d in kids:
        print("   ", d.unique_id(), "zone", d.links["zone"],
              "module", d.links.get("module"), "-> via", d.ha_device_block().get("via_device"))
    print("\n== thermostat 의 boiler 는 항상 1 (실제 또는 가상) ==")
    for d in kids:
        print("   ", d.unique_id(), "-> boiler", d.links["boiler"], "(실제 f06)")
    print("   ", solo.unique_id(), "-> boiler", solo.links["boiler"],
          "via", solo.ha_device_block().get("via_device"), "(가상)")

    print("\n== 가상 boiler: thermostat 에서 온도·온수 제어 노출 ==")
    print("  ", vboiler, " module:", vboiler.links.get("module"))
    print("   제어 가능 속성:", vboiler.command_attrs())
    print("   온수 설정 명령 토픽:", vboiler.topic("hw_temp") + "/set")
    print("   난방 온도 설정 토픽:", vboiler.topic("heat_temp") + "/set")
    print("\n== 샘플 토픽/명령 ==")
    print("  boiler power 명령:", boiler.topic("power") + "/set")
    print("  thermostat target:", t1.topic("target"))
    print("  oven ha device:", oven.ha_device_block())
