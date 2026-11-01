# CZ-N706B Arduino Library

Shared AT-command, power-control and communication support for the CodeZoo
CZ-N706B modem, with **21 examples for each of three boards**.

- [Developer Guide and AT command reference](https://www.codezoo.co.kr/n706b.html)
- [Arduino practice guide](https://www.codezoo.co.kr/n706b.html#arduino)
- [Library metadata](library.properties)

## Supported boards and wiring

Install the matching Arduino board package and select the board below.
Use the example directory for that board; each sketch is a separate example.

| Setting | UNO R4 | ESP32 | ESP32-S3 |
| --- | --- | --- | --- |
| Arduino IDE board | Arduino UNO R4 Minima | ESP32 Dev Module | ESP32S3 Dev Module |
| Examples | `examples/UNO_R4/` | `examples/ESP32/` | `examples/ESP32S3/` |
| TX (host → modem) | D1 / Serial1 TX → modem RX | GPIO17 / UART2 TX → modem RX | GPIO38 / UART2 TX → modem RX |
| RX (host ← modem) | D0 / Serial1 RX ← modem TX | GPIO16 / UART2 RX ← modem TX | GPIO37 / UART2 RX ← modem TX |
| ON_OFF | D2, active HIGH pulse | GPIO18, active HIGH pulse | GPIO2, active HIGH pulse |
| LDO_EN | D8, active HIGH | GPIO5, active HIGH | GPIO1, active HIGH |
| IOREF | HOST I/O 5V | HOST I/O 3.3V | HOST I/O 3.3V |
| GND | Common GND | Common GND | Common GND |
| Modem supply | Regulated 5V, ≥2A recommended | Regulated 5V, ≥2A recommended | Regulated 5V, ≥2A recommended |
| PC / modem serial speed | 115200 / 115200 baud | 115200 / 115200 baud | 115200 / 115200 baud |

IOREF specifies the host I/O voltage, not the modem supply voltage. Use the
appropriate CodeZoo board interface and check supply and backfeed wiring.

ESP32 and ESP32-S3 explicitly instantiate UART2 instead of relying on the
core's default `Serial1` or `Serial2` pin mappings:

```cpp
HardwareSerial MDMSerial(2);

// ESP32: RX GPIO16, TX GPIO17.
MDMSerial.begin(MODEM_BAUD, SERIAL_8N1, 16, 17);

// ESP32-S3: RX GPIO37, TX GPIO38.
MDMSerial.begin(MODEM_BAUD, SERIAL_8N1, 37, 38);
```

These are alternative initialization lines; use the one in your board's
example. UNO R4 uses `Serial1.begin(MODEM_BAUD)`.

For ESP32-S3, select **USB CDC On Boot: Disabled** and use the USB-to-UART
connection for PC `Serial`. The current shared PC logging API accepts
`HardwareSerial`; native USB CDC enabled configurations are outside this target.

## Install and run

1. Install the Arduino Renesas UNO or Espressif ESP32 board package for your board.
2. Download the complete library ZIP, including `src/`, `examples/` and `tools/`.
3. In Arduino IDE, select **Sketch > Include Library > Add .ZIP Library...**.
4. Open **File > Examples > CZ-N706B-Arduino**, select your board directory,
   and start with **CheckModule**.
5. Select the matching board and port, then upload.
6. Open Serial Monitor at **115200 baud**. CheckModule accepts single menu keys
   and ignores CR/LF. Automatic examples need no command input.

For TransparentBridge, select **Carriage Return**, enter an AT command and
press Enter. Both NL & CR is also accepted by the modem. When using QCOM,
enable **DTR**. Close Serial Monitor/QCOM before opening a PC tool on the same port.

The library depends on the selected Arduino board core. PC tools additionally
require Python 3 and the dependencies in `tools/requirements.txt`.

## Package layout

```text
CZ-N706B-Arduino/
├── library.properties
├── README.md
├── src/                  Shared .h/.cpp modules
├── examples/
│   ├── UNO_R4/           21 board-specific sketches
│   ├── ESP32/            Same example names
│   └── ESP32S3/          Same example names
└── tools/
    ├── requirements.txt
    ├── file_transfer.py
    ├── certificate_manager.py
    └── README_FileTransfer.md
```

## Examples

Each entry is available as `examples/<Board>/<Name>/<Name>.ino`.
The numbers match the online practice guide.

| No. | Example | Purpose |
| --- | --- | --- |
| 01 | CheckModule | Power on, query AT/model/firmware, normal shutdown and restart using `h/i/o/f`. |
| 02 | TransparentBridge | Forward PC and modem UART bytes for manual AT-command practice after AT OK. |
| 03 | GenericCommand | Automatically repeat AT, CGMM, IVSN, CPIN? and CSQ; wait 1 second after each result. |
| 04 | NetworkInfo | Prepare the network and periodically display SIM, registration, operator, signal, band and IP. |
| 05 | NetworkConnect | Connect, disconnect, reconnect and query status using `c/d/r/s/h`. |
| 06 | NetworkMonitor | Monitor registration, signal and IP after connection; report loss without automatic reconnection. |
| 07 | DNS_Check | Prepare the network and resolve the configured hostname. |
| 08 | FileSystem | Write, append, read, rename and delete reserved sample files. |
| 09 | FileTransfer | Use the PC menu to list, upload, download and delete modem files. |
| 10 | ConfigFile | Load, edit, save and verify settings stored as `config.ini`. |
| 11 | CertificateManager | Use the PC menu to list, register, check and delete a named certificate. |
| 12 | TCPClient | Send an application payload to a TCP echo server and verify the response. |
| 13 | UDPClient | Send an application payload to a UDP echo server and verify the response. |
| 14 | SSLTCPClient | Use TLS server authentication to request and validate an HTTP response over SSL TCP. |
| 15 | GMailClient | Synchronize time, connect with TLS, authenticate and send one Gmail SMTP message. |
| 16 | HTTPClient | Execute HTTP GET followed by POST once. |
| 17 | HTTPSClient | Execute HTTPS GET followed by POST using an installed CA certificate. |
| 18 | HTTPDownload | Download 10240 bytes to the modem filesystem and read back the saved file. |
| 19 | MQTTClient | Connect, subscribe, publish and receive a PC-published message using plain MQTT. |
| 20 | AWS_IoT | Use MQTT mutual TLS with an AWS endpoint and installed CA/client certificate/private key. |
| 21 | TimeSync | Request a modem time update, read the clock and confirm IP release. |

Normal operation of examples 01–21 has been confirmed by user testing on
UNO R4, ESP32 and ESP32-S3. This does not imply exhaustive fault or endurance
testing. Detailed execution logs in the online guide remain the original
UNO R4 records unless explicitly labeled otherwise.

## Settings to review before uploading

Edit the named constants at the top of the selected sketch:

- Board UART/GPIO settings and power timing.
- SIM APN, server hostname, port, path and test payload where applicable.
- Certificate names stored in the modem, MQTT topics/client ID and credentials.
- Timeouts and test counts. Socket/MQTT examples use `testCount = 1` by default;
  setting it to zero disables their test sequence.

Network examples assume an activated CodeZoo Vodafone IoT SIM and antenna;
the default APN is `simplio.apn`. Use your service's APN for another SIM.
Network requests consume SIM data. Resetting or uploading can repeat a test.
Do not publish configured passwords, private keys or personal account settings.

## Common power sequence

Initialize safe GPIO levels with `ModemPower_begin(&POWER_CONFIG)`, start the
PC and modem serial ports, and initialize `ModemAT_begin(...)` before calling
`ModemPower_powerOn()`. Follow the complete initialization in the chosen sketch.

Power-on defaults:

1. Set LDO_EN HIGH and wait 100 ms on initial activation.
2. Set ON_OFF HIGH for 1100 ms, then LOW and wait 100 ms.
3. Drain startup RX within a 500 ms deadline, requiring 20 ms of quiet, and
   reset the shared receive state. `Serial.flush()` does not clear RX.
4. Send AT every 1000 ms during the default 20000 ms boot observation window.
   A final OK completes readiness immediately.

Without OK, a normal boot timeout requires **at least 10 AT attempts**. Even
if a shorter boot window is configured, the shared function completes those
attempts and allows a response interval after the last required probe.
Each boot attempt and its final count are logged. RX overflow or a blocked
command boundary can stop early. Only AT is retried; ON_OFF is not pulsed again.
No AT probe or receive polling runs during the LDO delay, ON pulse or release delay.

Normal shutdown first checks AT communication, then applies ON_OFF HIGH for
3100 ms, LOW and a 100 ms release delay. The default observation lasts 10 seconds,
sends AT every second, requires at least three probes, and checks for at least
three seconds without valid AT responses. Unrecognized partial bytes remain in
diagnostics; final errors, ready URCs or receive-buffer errors prevent confirmation.

`MODEM_POWER_OFF_AT_SILENT` is an **AT-response-based shutdown confirmation**.
LDO_EN stays HIGH and PWRMON is not connected, so physical power removal is not
confirmed. An already-responsive modem is not given another ON pulse. An
unresolved previous power state must pass the shutdown recheck before re-powering.

## Common network preparation

Data protocol examples use this order before starting their protocol test:

1. Enable power and confirm the final AT OK.
2. Confirm `AT+CPIN?` returns `+CPIN: READY`.
3. Apply `AT+CGDCONT=1,"IP","<APN>"` and confirm OK.
4. Poll `AT+CEREG?` within a deadline until stat 1 or 5. For the Vodafone IoT
   SIM test environment, stat 5 means registered while roaming.
5. Query `AT+XIIC?`. Preserve an existing state-1 link with a valid IP. Otherwise,
   activate with `AT+XIIC=1` and query again to confirm state 1 and a valid IP.
   `0.0.0.0` does not indicate a ready connection.
6. Start the protocol only after all preparation checks succeed.

An AT command's OK alone does not prove registration, connection or transfer
completion. CheckModule, manual TransparentBridge and local file/certificate
management do not automatically perform SIM/APN/PPP preparation.

NetworkInfo can attempt link activation after a successful status query reports
no valid IP. NetworkMonitor reports loss without reconnecting. NetworkConnect
performs connection changes only through startup or its menu actions.

## PC tools

Upload the matching **FileTransfer** or **CertificateManager** sketch first.
From the extracted library root, install dependencies and run the required tool:

```bash
cd tools
py -3 -m pip install -r requirements.txt
py -3 file_transfer.py --port COM3
```

For certificates, run this instead:

```bash
py -3 certificate_manager.py --port COM3
```

COM3 is an example; use the board's actual port. Keep both Python scripts in
the same directory because CertificateManager reuses FileTransfer's transport.
DTR is enabled by the tools. Only one application may use the port at a time.

| Tool | Menu | Verification |
| --- | --- | --- |
| FileTransfer | `h` help, `l` list, `u` upload, `d` download, `x` delete, `q` quit | Upload read-back SHA-256 comparison; downloaded file hash; named deletion confirmation. |
| CertificateManager | `h` help, `i` list, `u` upload, `x` delete, `q` quit | Write completion followed by `AT+CERTCHECK="<name>"`; named deletion and absence check. |

CertificateManager checks startup readiness with AT/OK followed by CGMM/model/OK,
keeping one command outstanding. Startup `+PBREADY` is logged without replaying
an in-flight command. The startup deadline is 30 seconds and a quiet-boundary
check rejects leftover responses before the menu.

Certificate upload uses the selected file's exact byte count, waits for CONNECT,
then sends bytes without conversion or extra CR/LF. The tool accepts CONNECT
and named completion tokens without a trailing newline. It does not read back
certificate bytes or compare hashes. There is no certificate download or
delete-all action. `q` closes the PC port and leaves files/certificates stored.

See [FileTransfer tool instructions](tools/README_FileTransfer.md) for details.

## Certificates and time

Certificate names in sketches refer to **modem storage**, not PC paths. Install
the required files using CertificateManager before running a TLS example.

| Example | Required modem certificate/settings |
| --- | --- |
| SSLTCPClient / HTTPSClient | Download [ISRG Root X1](https://letsencrypt.org/certs/isrgrootx1.pem) and register it as `httpbin_ca.pem`, matching `CA_FILE`. |
| GMailClient | Download [GTS Root R1](https://i.pki.goog/r1.pem) and register it as `gts_root_r1.pem`. |
| AWS_IoT | Register your `aws_ca.pem`, `aws_cert.pem` and `aws_key.pem`; configure your endpoint, client ID and AWS permissions. |

`CERTCHECK` success confirms certificate registration/checking, not trust in a
particular server. Servers may change certificate chains. Keep the selected CA
and modem clock appropriate for the destination; authentication is not disabled
automatically when TLS fails.

SSLTCPClient and HTTPSClient require the correct modem clock before running;
printing CCLK does not independently validate its accuracy. HTTPS uses its own
HTTPSCFG settings; SSL TCP settings do not replace them.

GMailClient disables command echo with ATE0, prepares the network once, then uses
the active IP for an E0/DST-0 time update. It writes the validated UTC result to
CCLK and checks readback before opening `smtp.gmail.com:465`. Enter a plain email
address and app password in the sketch; Base64 authentication is handled by the
SMTP module and is not printed. The account must permit app-password login.

GMailClient sends one message per reset. Final DATA 250 means the server accepted
the message, not that inbox delivery was independently confirmed. After acceptance,
peer closure following QUIT may permit IP release without an observed SMTP 221;
the log distinguishes these outcomes. Mail is never automatically resent.

Standalone TimeSync defaults to E9/DST 0 and prints raw clock data. Its
`CLOCK_FIELDS_ARE_UTC` setting controls display conversion for the firmware's
clock format. It does not call `ModemTime_applySyncedClock()` or set CCLK directly.

## Shared modules and UART ownership

| Module | Responsibility |
| --- | --- |
| [ModemAT](src/ModemAT.h) | Bounded AT transactions, response values, URC observation, prompt/data writes, length-framed file reads and raw-mode handoff. |
| [ModemPower](src/ModemPower.h) | Shared power pulse timing, boot AT retries and AT-response-based shutdown observation. |
| [ModemSocket](src/ModemSocket.h) | Network preparation and TCP/UDP/TLS open, send, receive, close and IP release. |
| [ModemHttp](src/ModemHttp.h) | HTTP/HTTPS preparation, GET/POST, response parsing and download to modem storage. |
| [ModemMqtt](src/ModemMqtt.h) | MQTT/MQTTS setup, subscription, publication, receive handling and disconnect. |
| [ModemSmtp](src/ModemSmtp.h) | SMTP reply parsing, authentication, message formatting and one-message submission over ModemSocket. |
| [ModemTime](src/ModemTime.h) | Modem time queries/update results and optional validated UTC clock application. |

Only one component may consume the modem UART at a time. Raw-mode clients take
ownership after power-on; do not mix shared AT, socket, HTTP or MQTT operations
on the same UART. Restart the sketch before returning from raw mode to the
line-based AT interface. GMailClient's socket-backed time API deliberately
reuses the socket parser rather than adding a second UART reader.

`ModemAT_execute()` requires the final OK and, when requested, the expected
data prefix. `ModemAT_copyResponseValue()` copies the current value to a caller
buffer. It reports no value or insufficient capacity rather than returning a
truncated value or data from an earlier command. Line observers inspect data;
they must not print, send commands or re-enter the parser.

TransparentBridge uses 256-byte buffers in each direction, short collection
delays and preservation of unwritten bytes. It forwards the original byte stream;
it does not automatically issue commands after readiness or enable TCPTRANS/UDPTRANS.
For binary file transfers, use the dedicated PC tools and their verification.

## Transfer behavior and limits

- TCPClient/UDPClient use modem-buffered reception and explicit READ commands.
  UDP send success is not proof of remote delivery; callers must handle their
  own message framing and validation.
- SSLTCPClient uses direct TLS receive frames and validates an HTTP 200 response
  with Content-Length. Its bounded text response does not support chunked,
  compressed or redirect responses. GMailClient separately uses buffered TLS
  reads in 32-byte chunks.
- HTTPClient targets `postman-echo.com:80`; HTTPSClient targets `httpbin.org:443`.
  Each runs GET then POST once. Their text parser supports Content-Length and
  chunked bodies, up to 4095 body bytes. Compressed, close-delimited and 1xx
  responses, and binary NUL in the text body, are unsupported. PASS means a
  complete HTTP 200 response, not automatic JSON field validation.
- HTTPDownload targets `http://dl.testfile.cc/10kb.dat` and saves 10240 bytes as
  `http_10k.bin`. By default it deletes only that target if already present.
  It checks size and reads the entire file back, but does not compare a source
  hash or measure speed. The completed file remains stored until the next run.
- MQTTClient uses `broker.emqx.io:1883`, publishes to `n706ea/topic/put` and
  waits for MQTT Explorer to publish to `n706ea/topic/get`. Use distinct client
  IDs. The shared receiver holds one pending message up to 2048 bytes; the
  quoted publish implementation accepts up to 512 bytes and rejects quotes
  and CR/LF. These are library limits, not modem specification limits.
- AWS_IoT requires mutual TLS and endpoint/topic permissions. A modem restart
  during MQTT disconnect requires readiness/IP checks and connection settings
  to be reapplied before reconnecting. Follow the example's bounded recovery flow.
- GMailClient supports ASCII headers/body within its 512-byte complete-message
  limit. Attachments, HTML, OAuth and STARTTLS are not implemented.

FileSystem removes only its reserved `sample.txt`/`archive.txt` test files.
ConfigFile uses `config.ini`, `config.tmp` and `config.bak`; inspect recovery
files when saving is blocked. Invalid existing configuration is not silently
overwritten. Explicit save can replace its temporary file. The backup/rename
sequence does not guarantee atomic recovery from power loss.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No boot AT OK | Verify board selection, UART pins, 115200 baud, IOREF, power supply and ON_OFF/LDO_EN wiring. Inspect the actual retry count and RX diagnostics. |
| SIM not READY / no LTE registration | Check SIM activation, antenna, coverage and APN. Command OK alone does not mean registered. |
| XIIC state 0 or IP `0.0.0.0` | Follow the bounded link activation/confirmation sequence before protocol commands. |
| TLS AUTHFAIL | Check modem time, destination hostname, selected CA/chain and TLS settings. Certificate registration alone is insufficient. |
| PC tool cannot connect | Upload the matching bridge sketch, select the actual port and close other serial applications. |
| TIMEOUT / OVERFLOW / BLOCKED | Inspect the last complete response and framing diagnostics. Do not send unrelated commands or discard RX to guess recovery. |
| Mail already accepted but cleanup failed | Inspect socket/IP state; do not automatically resend the message. |

Protocol requests and destructive operations are not automatically replayed on
failure. Cleanup is performed only when response boundaries and connection state
allow it; a failed run may leave a file, socket or active IP link to inspect.
The explicit boot AT retry policy and each example's documented polling/menu
behavior are separate from automatic protocol replay.
