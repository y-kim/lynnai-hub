/* 온습도계 카드: 고른 센서에 맞춰 "어느 핀을 어디에" 표를 다시 그린다(관리 화면에 그대로 들어간다 — hub_plugins.c).
 * 센서 순서는 hub_sensors.c 의 RS_* 와 같다. 셋 다 I2C 라 같은 네 선에 나란히 이어도 된다. pins = [모듈에 적힌 이름(모듈마다 다른 이름은 · 로 나란히), 허브 쪽]
 * 허브 쪽의 'sda'·'scl' 은 아래 입력 칸의 GPIO 번호로 바뀐다. nc = 모듈에 있지만 잇지 않는 핀. */
(function () {
  var W = [
    { pins: [['VIN', '3V3'], ['GND', 'GND'], ['SCL', 'scl'], ['SDA', 'sda']], nc: 'ADR · ALR · RST',
      note: '보드형 모듈은 저항이 이미 있습니다. I2C 주소 0x44.' },
    { pins: [['VIN · VCC', '3V3'], ['GND', 'GND'], ['SCK · SCL', 'scl'], ['SDI · SDA', 'sda']], nc: '3Vo · CS(CSB) · SDO',
      note: '모듈에 따라 이름이 다릅니다: SCK = SCL, SDI = SDA. CS 는 비워 두어야 I2C 로 동작합니다. 주소(0x76·0x77)는 자동으로 찾습니다. BMP280 이면 온도만 나옵니다.' },
    { pins: [['VIN', '3V3'], ['GND', 'GND'], ['SCL', 'scl'], ['SDA', 'sda']], nc: '3Vo · INT1 · INT2',
      note: '기압·온도 센서라 온도만 나옵니다(습도 없음). I2C 주소 0x60.' }
  ];
  document.querySelectorAll('select[name=type]').forEach(function (sel) {
    var f = sel.form, sda = f.querySelector('[name=sda]'), scl = f.querySelector('[name=scl]');
    if (!sda || !scl) return;
    var box = document.createElement('div');
    sel.insertAdjacentElement('afterend', box);
    function draw() {
      var w = W[sel.value] || W[0], td = '<td style="padding:5px 10px 5px 0">';
      var h = '<table style="border-collapse:collapse;margin:10px 0 4px"><tr style="color:#888;font-size:13px">' + td + '센서 모듈의 핀</td>' + td + '</td>' + td + '허브 보드</td></tr>';
      w.pins.forEach(function (p) {
        var to = p[1] == 'sda' ? 'GPIO ' + sda.value : p[1] == 'scl' ? 'GPIO ' + scl.value : p[1];
        h += '<tr>' + td + p[0] + '</td>' + td + '→</td>' + td + '<b>' + to + '</b></td></tr>';
      });
      h += '<tr style="color:#888">' + td + w.nc + '</td>' + td + '</td>' + td + '연결 안 함</td></tr></table>' +
        '<p class=muted style="margin:0 0 6px">선은 ' + w.pins.length + '개만 잇습니다. ' + w.note + '</p>';
      box.innerHTML = h;
    }
    sel.addEventListener('change', draw);
    sda.addEventListener('input', draw);
    scl.addEventListener('input', draw);
    draw();
  });
})();
