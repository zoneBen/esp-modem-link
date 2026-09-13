# esp32c3_test

Subscribes to an MQTT broker over an **ML307** on an ESP32-C3 board, prints
everything that arrives on the topic, and publishes a heartbeat to a second
topic once a second — using [esp-modem-link](../../) over a real UART.

## Wiring

| Signal | ESP32-C3 pin |
|--------|--------------|
| Module TX → chip RX | GPIO5 |
| Module RX ← chip TX | GPIO4 |

UART1 carries the modem. UART0 stays the console on GPIO20/21: the platform
layer refuses to take over a port another driver already owns, so moving the
modem onto UART0 would only fail.

## Configuration

Everything adjustable lives in one block at the top of
[main/esp32c3_test.cpp](main/esp32c3_test.cpp):

| Constant | Default | Notes |
|----------|---------|-------|
| `kApn` | `"cmnet"` | **Carrier-specific.** China Mobile's, which is what the ML307 here has been tested against. Change it for any other carrier or the module never reaches the network. |
| `kMqttHost` / `kMqttPort` | `cloud.mrcong.cn` : `1883` | |
| `kMqttTopic` | `"test/"` | Subscribed at QoS 1. The trailing slash is part of the filter. |
| `kHeartbeatTopic` | `"test2/"` | Published to at QoS 0, once per `kLoopTickMs`. |
| `kMqttUser` / `kMqttPass` | empty | Empty means anonymous. Fill in **both** if the broker wants credentials — the protocol requires a user name to accompany a password. |
| pins / baud / UART | UART1, GPIO4/5, 115200 | |

## Build and flash

```powershell
# from the repository root
.\scripts\idf.ps1 -p COM7 flash monitor
```

Every argument goes to `idf.py` verbatim, so the usual
`build` / `flash` / `monitor` / `menuconfig` subcommands and flags all work.
From Git Bash use `./scripts/idf.sh` instead — same arguments.

To keep the environment in your own shell rather than build immediately:

```powershell
. .\scripts\idf.ps1
idf.py build
idf.py -p COM7 flash monitor
```

## What it does

Stages print as they pass, so a failure says where it stopped:

1. **create** — builds the ML307 HAL and opens UART1, sending nothing yet
   (`Create` names the type, so no `AT+CGMR` goes out; swap in `Detect` to have
   the module identify itself).
2. **identity** — revision, IMEI, ICCID, carrier.
3. **sim + network** — SIM state, registration state, signal strength.
4. **wait for registration** — up to 30 s.
5. **mqtt** — brings up the PDP context, connects, subscribes to `kMqttTopic`,
   then logs every message and publishes heartbeats until reset.

Each message is logged as a header line plus the payload:

```
I (41230) c3_test: message  topic='test/' qos=1 11 bytes
I (41230) c3_test: hello world
```

and each heartbeat the other way:

```
I (42000) c3_test: heartbeat -> test2/: uptime 42s
```

The app holds the session open indefinitely, printing `alive (uptime n s)` every
30 s. If the link drops, or the broker refuses the subscription, it reconnects
and re-subscribes on its own — leaving it running through a signal loss needs no
reset.

Publish to it, and watch the heartbeats, from anywhere:

```bash
mosquitto_pub -h cloud.mrcong.cn -p 1883 -t test/ -m "hello world"
mosquitto_sub -h cloud.mrcong.cn -p 1883 -t test2/
```

## Notes

- MQTT runs as the library's software client over a TCP socket — no module in
  this repository has an MQTT stack of its own.
- Port 1883 is plaintext, which keeps this clear of TLS: this firmware ships no
  certificate authority, so an 8883 session would fail on the certificate
  rather than on the broker.
- Printed payloads are capped at 400 bytes. The console runs at 115200, so
  echoing a large message in full would block the receive task for seconds while
  the module keeps sending into a 4 KiB UART ring buffer — and what overflows
  there corrupts the AT line stream, not just the message.
- `subscribe` is logged as sent, not accepted. The SUBACK comes back later, and
  a refusal reaches `OnError` rather than the return value.
- The heartbeat is QoS 0, so it is fire-and-forget: nothing answers it, so it can
  never stall the loop on a PUBACK a dead link will not send. A dropped heartbeat
  is not retried — the next one is a second away, and says the same thing.
- The heartbeat period is the tick delay *plus* the time the publish takes. That
  is deliberate: on a slow link the heartbeat thins out rather than the loop
  falling behind and then firing a backlog to catch up. Both directions share one
  session, so a heartbeat that fails raises the same re-subscribe path a dropped
  link does.

## Building for another target

`IDF_TARGET` selects the chip, defaulting to `esp32c3`:

```powershell
$env:IDF_TARGET = "esp32s3"; .\scripts\idf.ps1 build
```
```bash
IDF_TARGET=esp32s3 ./scripts/idf.sh build
```

Only for a tree with no `sdkconfig` yet. Against one already configured for a
different chip, idf.py refuses rather than switching, so the script stops and
tells you to run `idf.py set-target <target>` instead (which discards the
existing `sdkconfig`).
