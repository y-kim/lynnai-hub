# 드라이버 가상 모사 (connection-path simulation)

실제 `../src/core.lua` 를 SmartThings Edge API 모의 위에서 돌려, **하드웨어·재페어링 없이**
보일러·써머스탯 여러 대, 수동 짝, 단독 써머스탯을 시험한다. 허브가 보내는 메시지(hub_uplink.h)를
바이트로 만들어 `core.on_message` 에 먹이고, core 가 만든 기기·보낸 값·기기로 보낸 명령을 검사한다.

```sh
python3 run_sim.py      # lupa 필요
```

`sim.lua` 의 시나리오: ①보일러1+써머스탯1(암묵 연동) ②보일러2대+써머스탯1(짝 없음→깨움 안 함)
③짝 지정→그 보일러만 깨움 ④단독 써머스탯. 새 시나리오는 sim.lua 에 추가.
