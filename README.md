# CZ-N706B Arduino library

This package contains the common UART/AT-response and power-control code used by the verified UNO R4 `CheckModule` example. It targets the UNO R4 core (`renesas_uno`) and includes ESP32 Dev Module and ESP32S3 Dev Module counterparts for examples 01–21. Examples 01–21 have been confirmed by user board testing on UNO R4, ESP32 and ESP32-S3.

## Install and open the example

1. Download the complete library ZIP. Do not download only `CheckModule.ino`; it requires the files in `src/`.
2. In Arduino IDE, select **Sketch > Include Library > Add .ZIP Library...**, then select the ZIP.
3. Open **File > Examples > CZ-N706B-Arduino**, choose **UNO_R4**, **ESP32** or **ESP32S3**, then open **CheckModule**.
4. Select the matching UNO R4 board, ESP32 Dev Module or ESP32S3 Dev Module and its port, then upload.
5. Open Serial Monitor at 115200 bps.

## Settings

`examples/UNO_R4/CheckModule/CheckModule.ino` contains the UNO R4 UART, GPIO, and timing settings:

- PC diagnostic UART: `Serial`, 115200 bps
- Modem UART: `Serial1`, 115200 bps
- `ON_OFF`: D2, active HIGH
- `LDO_EN`: D8, active HIGH
- power-on pulse: 1100 ms; power-off pulse: 3100 ms

The example keeps `LDO_EN` HIGH after its AT-silence shutdown check. Because PWRMON is not connected, that check confirms loss of AT communication only; it does not prove physical power removal.

## Initialization and response values

Call `ModemPower_begin(&config)` first to set the safe GPIO levels, then start the PC and modem UARTs, then call `ModemAT_begin(modemUart, pcUart, quietMs)`. Check the power initialization result and ModemAT_isInitialized() before calling the AT or power APIs.

`ModemAT_execute(command, expectedPrefix, timeoutMs)` retains its existing result API. When it returns `MODEM_AT_OK` for a command with an expected prefix, use `ModemAT_copyResponseValue(buffer, bufferSize)` to copy the trimmed value after that prefix. It never exposes an internal pointer and returns:

- `MODEM_AT_VALUE_OK`: complete value copied and NUL-terminated.
- `MODEM_AT_VALUE_NONE`: the last command did not produce a usable expected value.
- `MODEM_AT_VALUE_BUFFER_TOO_SMALL`: destination is null, has zero size, or cannot hold the complete value. No truncated value is returned.

Every new `ModemAT_execute()` clears the previous stored value before it validates or sends the command. A failed command therefore cannot leave an earlier value available as a current result.

The first power-on and an explicit re-power after an AT-silence shutdown use this sequence: ON_OFF LOW, LDO_EN HIGH, the configured LDO delay, one ON_OFF HIGH pulse, ON_OFF LOW, the configured release delay, `ModemAT_prepareAfterPowerOn()`, and then the boot AT observation. No AT probe or `ModemAT_poll()` runs from LDO enable through the end of the release delay. A normal `o` request while the modem is already running keeps its AT probe and does not pulse ON_OFF when AT is confirmed.

`ModemAT_prepareAfterPowerOn(drainTimeoutMs, quietMs)` is only for the first power-on or an explicit re-power. It discards physical UART RX bytes with a bounded drain window, checks the same total deadline while it drains a continuously available RX stream, then resets the RX queue, partial line, discard state, pending-command state, overflow state, trace capture, and stored response value. It returns `MODEM_AT_TIMEOUT` unless RX reaches the requested quiet interval before the drain deadline. Do not call it before ordinary commands or during shutdown observation, because it intentionally discards URCs and response data. `ModemAT_resetAfterVerifiedPowerState()` uses the same bounded reset for an independently verified known-power recovery; neither function synchronizes an active connection. `ModemAT_commandBoundaryReady()` reports whether a completed AT observation left a safe boundary for the next command.

`ModemPower_powerOff()` returns a `ModemPowerOffResult`. `MODEM_POWER_OFF_AT_SILENT` means the configured AT-silence criterion passed; it is not proof of physical power removal.

During the shutdown observation, the library records final `OK` responses, final error responses, recognized ready URCs, and every received UART byte separately. `MODEM_POWER_OFF_AT_SILENT` means the configured number of AT probes completed with no final `OK` or final error response for the configured response-absence interval. CR/LF-free partial data and unrecognized bytes remain in diagnostics but do not reset that response timer. A recognized ready URC or any receive-buffer error prevents confirmation, because it leaves the power state ambiguous. When the valid-AT-response boundary is confirmed, the remaining partial line and pending AT state are cleared. If shutdown was previously unconfirmed, `ModemPower_powerOn()` first performs the same bounded response-absence recheck; it sends an ON pulse only after that recheck confirms the shutdown criterion.

URCs are currently logged by `ModemAT_pollIdle()` as idle/URC text. No callback API is provided yet: the current receive buffer is reused internally, so a callback would require an explicit ownership and reentrancy contract.

The line-based AT parser is for this example's AT, `AT+CGMM`, and `AT+IVSN` exchanges. TCP/HTTP binary payloads and `>` prompts are outside this library's current scope.

## TransparentBridge

`examples/UNO_R4/TransparentBridge/TransparentBridge.ino` uses the same UNO R4 power configuration as CheckModule. It powers the modem on through `ModemPower_powerOn()`, waits for the final AT `OK`, then calls `ModemAT_enterRawMode()` once. In raw mode, `ModemAT_poll()`, `ModemAT_pollIdle()`, command execution, and power observation do not consume modem UART data.

Use `ModemAT_rawAvailable()` and `ModemAT_rawRead()` only after a successful raw-mode transition. They return pending parser partial-line bytes first, then queued UART bytes, then the hardware UART, preserving their observed order. Raw mode has no parser return API in this release; restart the sketch before using AT or power APIs again. TransparentBridge is a manual UART AT-terminal exercise. It does not enable TCPTRANS or UDPTRANS, and it does not promise lossless binary-file transfer under every host-output backpressure condition.

On Renesas UNO core 1.6.0, the UART class does not override `availableForWrite()`, so the inherited `Print` implementation reports zero. TransparentBridge therefore does not use `Serial1.availableForWrite()` for the modem direction. It sends at most `BRIDGE_STEP_BYTES` queued bytes per loop and removes a byte only after `Serial1.write()` returns one. The core's `UART::write(uint8_t)` waits for its transmit-empty callback, so the bounded step prevents one bridge direction from monopolizing the loop. The PC USB Serial direction continues to use its own `availableForWrite()` implementation.

## GenericCommand (UNO R4 / ESP32 / ESP32-S3)

Open `examples/UNO_R4/GenericCommand/GenericCommand.ino`. After power-on AT confirmation, the sketch automatically repeats AT, CGMM, IVSN, CPIN? and CSQ. No PC command input is used. Serial Monitor runs at 115200 baud. Each command starts at least 1000 ms after the previous result is reported; the first query also waits 1000 ms. This is not a fixed one-second start-to-start schedule. Each command has a 5000 ms timeout.

The query table stores each command and expected data prefix. A final modem ERROR or missing expected data is reported and the sequence continues. NO RESPONSE, TIMEOUT, OVERFLOW or BLOCKED stops the sequence without retries or buffer flushing. Idle URCs continue to be handled after stopping. Check diagnostics and the modem state before restarting. Prompt/data transfers and asynchronous service completion need dedicated examples.

## NetworkInfo (UNO R4 / ESP32 / ESP32-S3)

Use an activated CodeZoo Vodafone IoT SIM and antenna. The APN defaults to simplio.apn for this Vodafone IoT SIM test. Confirm it matches the SIM service before uploading. This example configures CID 1 as IP, waits for CPIN READY, registration (1 or 5), activates XIIC and verifies a nonzero IPv4 address. Registration waits up to 180 seconds; XIIC activation and IP confirmation each have a separate 60-second limit. No APN username/password is configured.

After connection, each snapshot queries CPIN, CEREG, COPS, CSQ, NETMSG, NETDMSG and XIIC. Wait 10 seconds after each snapshot. A successful XIIC? with state 0 or state 1 without a valid IP triggers one XIIC=1 attempt per snapshot, followed by up to 60 seconds of IP confirmation. XIIC=1 itself has a separate 60-second timeout. Failed or unrecognized XIIC? responses do not trigger reconnect. A completed activation error or IP confirmation deadline is retried only in the next snapshot. Transport failures stop monitoring. The APN is not rewritten during monitoring. Failed fields show Unavailable. NETDMSG remains raw. Final errors/missing data allow other fields to be queried; transport failures stop monitoring. Compilation and normal example operation have been confirmed on all three boards.

## NetworkConnect (UNO R4 / ESP32 / ESP32-S3)

Use an activated CodeZoo Vodafone IoT SIM, APN simplio.apn, CID 1 and IP type. Startup powers on, confirms SIM READY, configures APN and connects. Menu: c connect, d disconnect, r confirmed disconnect then connect, s registration/link status, h help. Already-connected/disconnected links skip redundant control commands. Connect succeeds only with XIIC state 1 and a nonzero IPv4; disconnect requires state 0 and 0.0.0.0. No background reconnect occurs. Enter one key and wait for Menu ready. Keys buffered during operations are discarded.

Registration waits up to 180 seconds. XIIC control commands wait up to 60 seconds, followed by a separate 60-second status confirmation window. Ordinary queries wait 5000 ms. Final modem errors return to the menu; unresolved transport errors latch control blocked and require investigation before restarting. Status queries never activate a link. Disconnect affects all services using the internal link; close those services first. Compilation and normal example operation have been confirmed on all three boards.

## NetworkMonitor

Board-specific example: check SIM, configure APN, wait for registration, activate XIIC once and confirm IP. Then poll CEREG?, CSQ and XIIC? every 5000 ms after cycle completion. Link loss is logged without automatic reconnection. Setup failure prevents monitoring. English comments and logs. Normal example operation has been confirmed on all three boards.


## FileSystem

Runs once without SIM, APN or network activation. Before testing, lists and deletes only existing sample.txt and archive.txt, then confirms both names are absent. Writes HELLO, appends 123, verifies size and contents, reads HELL and O123, renames to archive.txt and deletes it. Other files remain untouched. Stops on failure without automatic cleanup. Reset to run again. Normal example operation has been confirmed on all three boards.

The common UART API adds ModemAT_executeWithHandler for synchronous line inspection and ModemAT_writeData for a prompt followed by exact payload bytes without CR/LF. Line handlers must not print or send commands. FileSystem reads short ASCII test data only, not arbitrary binary files.

## FileTransfer (UNO R4 / ESP32 / ESP32-S3)

Use the companion interactive PC tool: tools/file_transfer.py. Start it with the actual Arduino
port, then select h/l/u/d/x/q at FileTransfer>. Default blocks: write 256 bytes, read 32 bytes.
The tool lists files, uploads with read-back SHA-256 comparison, downloads and reports a hash,
and deletes a selected file after exact-name confirmation and absence verification. No SIM,
APN or network activation is required. See tools/README_FileTransfer.md for setup and usage.

## ConfigFile (UNO R4 / ESP32 / ESP32-S3)

ConfigFile stores version=1, device_id=N706_Client_01 and sample_interval_sec=10 in config.ini. Allowed intervals are 10/30/60 seconds; device_id allows 1..32 ASCII letters, digits, underscores and hyphens. Required keys must appear exactly once. Unknown keys, unsupported versions, embedded NULs and files outside 1..255 bytes are rejected. LF and CRLF lines, blank lines and # comments are accepted; no BOM. SIM/network is not required.

Menu: s show, i cycle interval in RAM, w save and verify, r reload, d RAM defaults, h help. RAM-only changes are explicitly marked unsaved. TICK logs show device id, count, interval and last verified save status. Reload failures keep RAM values but mark storage as unverified.

Missing config.ini is created only after a successful file listing and only when config.bak is absent. Existing invalid configuration is never overwritten automatically. Save writes reserved config.tmp, verifies exact bytes and parsed settings, renames the existing config.ini to config.bak, commits the temporary file and verifies again before deleting the backup. An existing config.bak blocks saving; inspect recovery files before retrying. Existing config.tmp is removed during an explicit save. This is not a guarantee of atomic recovery from power loss.

ModemAT_readFile reads the FSRF payload using its announced byte count, preserving CR/LF and final-like text inside the payload. Its expected size must match FSFS; accepted size is 1..512 bytes. A malformed/incomplete response can leave the command boundary blocked rather than silently discarding bytes. Existing AT functions are unchanged. ConfigFile was compiled for UNO R4 Minima and tested with a mock UART; normal example operation has been confirmed on all three boards.

## CertificateManager (UNO R4 / ESP32 / ESP32-S3 + PC)

Initial connection reads startup output for at least one second and waits for bridge-ready if boot activity is detected. After 250 ms of RX quiet, it completes AT/OK and then CGMM/model/OK with only one command outstanding. PBREADY is logged without restarting an in-flight query. A final 250 ms quiet check rejects leftover command responses before showing the menu. The single startup deadline is 30 seconds. Partial lines are not discarded, and certificate operations are never replayed automatically.


Upload examples/UNO_R4/CertificateManager/CertificateManager.ino. It uses the same raw byte bridge as FileTransfer. Install pyserial, close Serial Monitor and run `py -3 certificate_manager.py --port COM3` from tools/. Keep file_transfer.py beside certificate_manager.py because its serial transport is reused. DTR is enabled before opening the port. No SIM, APN or network is required.

Interactive menu: h help, i certificate list, u upload, x named deletion, q close the port. There is no download or delete-all action. PC input limits are 1 byte..64 KiB and 1..50 ASCII filename characters (letters, digits, dot, underscore, hyphen; no paths, dot or dot-dot). These are tool limits, not claimed modem maximums.

Upload snapshots the file as binary, reports its name/size, asks for registration or same-name overwrite consent, sends CERTADD with the whole byte count, waits for CONNECT and sends the original bytes in 32-byte bursts with 10 ms pauses. CONNECT is recognized even without trailing CR/LF. Write completion accepts CERTADD length/OK (requiring the matching length) or a standalone OK, followed by CERTCHECK=<name> success. Certificate completion tokens also do not require trailing CR/LF. No byte conversion, appended Enter, hash read-back or automatic retry is performed. Named responses can complete without a separate OK; optional trailing OK is handled before the next command.

Deletion lists certificates, requires one name and exact-name confirmation, then issues only CERTDEL=<name> and checks absence in the list. A bare CERTDEL is never constructed. q closes the port, retains certificates and does not power off the modem. A failure can leave a partial or replaced certificate; inspect the target before retrying. Registration verification does not establish TLS trust or server connectivity.

UNO R4 Minima compile passed; ten mock-serial tests cover data preservation, optional OK, menu dispatch, overwrite/delete cancellation, malformed acknowledgments, empty/invalid input, CONNECT gating, named verification and DTR/port closure. User board testing of the certificate workflow has been completed on all three boards.

CertificateManager regression update: twelve mock-serial tests now include CONNECT without newline, named completion without newline, and standalone write OK followed by named CERTCHECK. This fixes the case where the modem had already sent CONNECT but the PC waited for an LF before sending the certificate. No certificate read-back or filesystem commands are used.

## TCPClient, UDPClient and SSLTCPClient (UNO R4 / ESP32 / ESP32-S3)

Use an activated SIM, antenna, stable supply and the APN provided by the SIM service.
All three sketches set testCount=1. Each attempt opens one socket, sends one application
payload, verifies its response, and closes. Zero disables the test. No network commands are
issued from loop(); failures stop remaining attempts without automatic retry or reconnection.
Increasing testCount consumes more SIM data. DNS, TCP/TLS overhead and incoming responses
are additional to the request size. Resetting or uploading the board can start a new test.

TCPClient and UDPClient default to modules.telit.com:10510 and Hello (5 bytes), with modem-buffered
RECVMODE=0 reception and TCPREAD/UDPREAD.
SSLTCPClient uses TLS1.2/server authentication and GET /get on httpbin.org:443. Register the
matching CA as CA_FILE (default httpbin_ca.pem) and set the actual modem clock in advance.
The example checks only basic clock format/range, not its accuracy. TLS authentication is
never disabled on failure. HTTP completion requires status 200 and a Content-Length body;
chunked/compressed/redirect responses are not supported. Maximum response is 4095 bytes.

APN, SERVER_HOST, SERVER_PORT, SOCKET_ID, payload/path/CA, timeouts and testCount are named
settings at the top of each sketch. Current tests use dedicated modem/socket/link resources;
the examples deactivate XIIC after socket cleanup when command boundaries are known. If a
response boundary is unresolved, no additional cleanup command is sent. A socket/link may
remain and must be inspected before restarting.

ModemSocket handles asynchronous setup/send/close results and receive URCs by declared byte
length. Compile and native UART simulation results are separate from hardware tests.
Validation: 14 simulated cases passed; normal socket example operation has been confirmed on all three boards.

TCPClient and UDPClient use separate Open/Send/Recv/Close calls and typed connection
settings. sendData and testCount are sketch-owned; testCount defaults to 1.
APN is set before registration checks. CEREG is polled every 2 seconds with a deadline.
XIIC is queried before activation; confirmed IP is printed. Confirmed Close is followed
by XIIC=0 and IP-release confirmation. A failed Close never triggers IP release.

TCPClient/UDPClient use RECVMODE=0. RECV URCs only mark data pending. recv() issues
TCPREAD/UDPREAD outside the URC parser, captures exactly the returned byte count and
requires final OK. READ_CHUNK_BYTES defaults to 1024 (allowed 1..2048); the caller's
capacity also limits the request. A full read marks more data potentially pending;
a new notification during READ is retained. The sketch reads one block per attempt.
The caller handles further reads, message framing and content validation. recv() does
not compare sent content. UDP delivery and datagram boundaries must not be inferred
from send success or one READ response. READ timeout/partial framing blocks further
commands. Normal operation of these buffer-mode examples has been confirmed on all three boards.
SSLTCPClient uses ModemSocketTlsConfig and explicit Open/Send/Recv/Close.
RECVMODE=1 direct UART frames are captured by their declared byte length. recv() does not
interpret HTTP; the sketch assembles a bounded text response and validates HTTP 200
and Content-Length. A confirmed server Link Closed avoids a duplicate Close command.
CA and modem clock must be prepared; server authentication remains enabled.
Request data, HTTP path and testCount are separate from the TLS connection settings.
Normal operation of this TLS example has been confirmed on all three boards.

READ diagnostics report header recognition, payload bytes, remaining bytes, final OK,
modem errors and the last 128 received bytes in HEX. Prefixed READ errors are reported
immediately. Incomplete response boundaries block additional commands.

SSLTCPClient uses RECVMODE=1 and no SSLTCPREAD commands. The default HTTP request
uses Connection: close and is 59 bytes including CR/LF. The sketch assembles direct
UART data, checks HTTP 200 and Content-Length, then confirms server/local closure
and releases IP. HTTP_CONNECTION is a named setting; testCount defaults to 1.

The HTTP example matches Content-Length field names without case sensitivity.

The sketch calls ModemSocket_waitForClose() for up to CLOSE_URC_WAIT_MS (5000 ms). A peer Link Closed skips ModemSocket_close(); otherwise the sketch calls it after the wait. After a clean Close
error, SSLTCPSETUP? must confirm target-socket absence before IP release is allowed.
An open socket, invalid query or unresolved response is never treated as closed.

TLS HTTP console output is split into SERIAL_LOG_CHUNK_BYTES (16-byte) writes with
UART polling before/after each write. Peer closure is latched until the next open;
other socket IDs do not alter this connection. Close diagnostics are deferred until
available UART input is drained. Split URC lines retain parser state across calls.

## HTTPClient (UNO R4 / ESP32 / ESP32-S3)

Open examples/UNO_R4/HTTPClient/HTTPClient.ino. An activated Vodafone IoT SIM
uses APN simplio.apn by default. Power-on, SIM readiness, APN, LTE registration
and IP acquisition follow the TCPClient preparation sequence: CPIN READY,
CGDCONT, CEREG polling to status 1/5, XIIC? first, XIIC=1 only without a valid
IP, then XIIC? confirmation. No HTTP requests start until this completes.
UART transmission matches TCPClient byte-wise writes with flush after each byte;
console output is deferred until query bytes have been parsed.
These preparation steps precede one GET /get?test=123 and one POST /post to
postman-echo.com:80. POST_BODY defaults to {"test":"123"}; its 14-byte length
is calculated from the configured string. Setup-to-action and prompt-to-body
transitions have no deliberate delay. No extra CR/LF is appended to the body.

ModemHttp.h/.cpp owns the raw modem UART after ModemHttp_begin(). Do not call
other AT/socket APIs on the same UART until the sketch restarts. The config
must remain alive throughout use. Complete, uncompressed HTTP/1.x responses
with Content-Length or chunked framing are supported: maximum body 4095 bytes,
header storage 3072 bytes and line buffer 1024 bytes (including terminators).
Close-delimited, compressed and informational 1xx responses are not supported.
Body text is length-framed so embedded OK/URC-looking text is not interpreted
as modem control data. Binary NUL is unsupported by this text example.

GET must complete with HTTP 200 before POST is attempted. PASS means complete
HTTP 200 receipt, not JSON field validation; inspect the printed args/json echo.
The HTTP connection is confirmed closed before IP release. An unknown response
boundary blocks further commands. Requests, including POST, are never replayed
automatically. Reset/upload runs the test again and consumes SIM data.
Native simulated-UART tests and UNO R4 Minima compilation passed; normal HTTPClient operation has been confirmed on all three boards.

## HTTPSClient (UNO R4 / ESP32 / ESP32-S3)

Open examples/UNO_R4/HTTPSClient/HTTPSClient.ino. The common power, SIM, APN,
registration and IP sequence is the same as HTTPClient/TCPClient. HTTPS uses
httpbin.org:443: GET /get?test=val then POST /post with the 14-byte JSON body
{"test":"123"}, once per reset. Existing HTTPClient behavior is retained.

Reuse the httpbin_ca.pem already registered for SSLTCPClient. CA_FILE is the
modem certificate name, not a PC path. The example never uploads/deletes CA
files. ModemHttp_beginTls() selects HTTPS commands; prepareNetwork() checks
CERTCHECK, prints CCLK, applies HTTPSCFG sslversion=3 (TLS 1.2), authmode=1
(server authentication), cacert and verifies HTTPSCFG readback. Existing SSL
TCP TLS settings do not substitute for HTTPSCFG. Set the modem clock correctly
before running; merely receiving CCLK data does not validate the current time.
No client certificate/key is required for this server-authentication example.

HTTPSSETUP AUTHFAIL or any query/response failure stops the request sequence.
There is no authentication downgrade, TLS-version fallback or automatic replay.
The existing bounded HTTP response parser handles HTTPSRECV and HTTPSCLOSED
with the same Content-Length/chunked limitations described for HTTPClient.
PASS denotes complete HTTP 200 receipt; inspect args.test=val and json.test=123
in the printed response. Normal HTTPSClient operation has been confirmed on all three boards.

Named CERTCHECK replies complete with +CERTCHECK: name,OK; a separate final
OK is optional. HTTPS preparation accepts both forms, including a terminal
token without CR/LF after a bounded quiet interval, and requires the exact
configured certificate name. Missing/error/incomplete replies do not permit
HTTPS requests. No certificate is re-uploaded or removed.

## HTTPDownload (UNO R4 / ESP32 / ESP32-S3)

Open examples/UNO_R4/HTTPDownload/HTTPDownload.ino. Common power/AT, SIM, APN,
registration and XIIC/IP preparation precedes one download from
http://dl.testfile.cc/10kb.dat to http_10k.bin. Expected size is 10240 bytes.
Direct AT-command testing confirmed FILEHTTPSTAT 0,1,10240 and FSFS 10240 on
this server. Normal HTTPDownload operation has been confirmed on all three boards.
There is no speed measurement, automatic download retry or upload.

REMOVE_EXISTING_TARGET=true deletes exactly TARGET_FILE if present, then
confirms absence through FSLIST. Missing files skip deletion; deletion failure
or unconfirmed absence blocks downloading. Other files are preserved. FSLS
requires expected size plus a 1024-byte reserve. HTTPSETUP is followed by
FILEHTTPACTION=0,0,size,name. Command OK and FILEHTTPSTAT success/exact length
are both required. HTTP is closed, FSFS is checked and the entire saved file
is read in 32-byte FSRF blocks. The first 32 bytes are printed in HEX.

The example passes NULL for expectedPattern: full readback and file length are
checked, not source-byte identity or a hash. TestFile data must not be compared
to the httpbin a-z fixture. A non-NULL pattern remains available to other API
callers. DOWNLOAD_PATH, TARGET_FILE, EXPECTED_BYTES and timeouts are named
sketch settings. The successfully downloaded file remains until the next run.

Failed FILEHTTPSTAT (including 1416) stops parsing immediately. No automatic
retry, failure cleanup or deletion is attempted at an unknown UART boundary.
Any partial target is kept until a new explicit run performs configured target
cleanup. IP release is allowed only after a known HTTP closure.

## MQTTClient / UNO R4

Plain single-channel MQTT based on the manual example: broker.emqx.io:1883,
MQTTMUX=0 and MQTTTLS="sslmode",0. CLIENT_ID defaults to N706_Client_01; give
each modem and MQTT Explorer a different ID. USERNAME/PASSWORD default to empty;
credentials are redacted from command logs. Use only non-sensitive test data.

The sketch owns testCount (default 1), topics, message and QoS settings. Each run
prepares SIM/APN/registration/IP, verifies broker state, subscribes to
n706ea/topic/get, publishes Hello EMQX once to n706ea/topic/put, then waits up to
60 seconds for one PC-published message. MQTT Explorer must publish to the get
topic after [READY]. It should subscribe to put to confirm the modem publication.
No automatic publish retry, reconnect or test execution occurs in loop().

ModemMqtt.h/.cpp exposes prepareNetwork, connect, subscribe, publish, receive,
unsubscribe, disconnect and releaseIp. MQTTSUB payloads are length-framed and
may contain CR/LF/NUL. One pending message up to 2048 bytes and a topic up to
127 bytes are supported. The example's quoted MQTTPUB payload is limited to
512 bytes and rejects quotes/CR/LF; this is not the modem's publish size limit.
The receive API does not compare sent/received payloads or validate JSON.

An occupied receive slot, overflow or incomplete UART frame stops further
commands. MQTT must be confirmed disconnected before IP release. If PBREADY
occurs during disconnect, the example verifies AT readiness and XIIC state;
it does not reconnect. Existing active MQTT connections are preserved.
The simulator and UNO R4 compilation do not replace hardware validation.

## AWS_IoT / UNO R4

Set BROKER_HOST to your AWS IoT endpoint, without a scheme or port. Register
ROOT_CA_FILE, CLIENT_CERT_FILE and CLIENT_KEY_FILE using QCOM or CertificateManager
before running. The sketch never uploads certificates or prints key contents.
Enable the certificate and grant the required AWS client/topic permissions.
MQTT Explorer uses its own certificate selection and a different client ID.

TLS uses MQTTMUX=0, sslmode=1, authmode=1 and sslversion=3 (TLS 1.2).
connectAwsIoT reuses prepareNetwork and connectTls for first connection and
reconnection. APN precedes bounded registration polling at two-second intervals.
IP is activated only if missing, then confirmed. TLS files/settings are reapplied
on every MQTT connection. The initial exchange publishes before subscribing,
matching the AT reference. Reconnected sessions subscribe before publishing.
Each [READY] requires one PC message to the configured get topic within 60 seconds.

Defaults: testCount=1 and reconnectCount=1, hence two message exchanges.
These are finite test steps, not failure retries. SIM data is consumed on each
connection/TLS handshake/exchange. Set reconnectCount=0 to skip reconnection.
No automatic publish or reconnect occurs in loop(). Failure stops remaining tests.

ModemMqtt_disconnect(RESTART_WAIT_MS) observes UART for the configured interval
(default 10000 ms) after command completion before querying MQTT state. This
handles delayed PBREADY in the reference environment. prepareReconnect(0) then
checks AT readiness once; SIM, registration and IP are checked by prepareNetwork.
If the modem is still not ready or UART framing is incomplete, testing stops.
The final disconnect also observes restart, then confirms IP release. Unknown
response boundaries block cleanup commands. Existing connections are preserved.
The TLS API is restricted to authMode=1 and sslVersion=3 for this mutual TLS flow.
Normal AWS_IoT operation has been confirmed on all three boards.

## TimeSync

Open examples/UNO_R4/TimeSync/TimeSync.ino. The example prepares SIM/APN,
registration (two-second polls) and IP, reads CCLK before synchronization,
requests UPDATETIME once, waits for its terminal result and reads CCLK again.
It then confirms IP release. No repeat, reconnect or synchronization retry
occurs in loop(). One request is allowed per ModemTime_begin session.

Configure APN, TIME_SERVER, SERVER_TIMEOUT_SECONDS, TIME_ZONE, DAYLIGHT_SAVING
and the bounded wait settings at the top of the sketch. Defaults are
simplio.apn, time.windows.com, 10 seconds, E9 and DST disabled. Host result
wait defaults to 20000 ms, and must be at least server timeout in milliseconds
plus the query timeout. Firmware-specific supported server timeout ranges
should be checked with UPDATETIME=? when adjusting the setting.

Initial OK only accepts the request. A valid +UPDATETIME: Update To timestamp
is required for synchronization completion. Documented terminal failures
allow a subsequent IP-release attempt. Missing/partial/unknown results block
further commands, since a delayed time result may still arrive. The data link
may remain active on preparation or unknown-boundary failures. No cleanup is
claimed unless XIIC state 0 and IP 0.0.0.0 are confirmed.

CLOCK_FIELDS_ARE_UTC=true matches the AT reference firmware
N706B-A08-STD-BZ_EA1X-006, which returned UTC clock fields with +36 for UTC+9.
The parser applies the returned 15-minute offset once for local display,
including month/year/leap-day rollover. Set false for firmware that already
returns local fields. Raw CCLK responses are always printed. UPDATETIME
notifications are displayed as returned, without an additional offset.
The example accepts quoted yy/MM/dd,hh:mm:ss+zz or -zz clock responses,
interpreting years as 2000-2099. Invalid dates or offsets are not converted.

An invalid initial clock is diagnostic; a valid post-sync clock is required
for PASS. PASS confirms the terminal update notice, RTC query and IP release;
it does not measure clock accuracy against an independent reference.
Startup PBREADY is accepted through the initial CPIN READY check. Before APN
configuration, this startup window closes; later PBREADY blocks commands as a
modem restart. No APN command or time update is replayed.
CCLK direct setting and CTZU are not modified. SIM data is consumed.
ModemTime.h/.cpp provide independent time and network support, without a
MQTT connection. Simulator checks and compilation do not replace board tests.

TimeSync uses the same ModemAT_begin and ModemPower_powerOn initialization as
NetworkMonitor and DNS_Check. ModemTime_begin(config) keeps the shared AT engine;
it does not enter raw mode or transmit UART bytes independently. ModemAT_execute
handles SIM/APN/CEREG/XIIC/CCLK, with PC logging after active response parsing.
ModemAT_executeWaitUrc retains UPDATETIME results received before or after the
request OK. Missing terminal results block further commands. The line observer
is inspection-only and detects startup/restart/link-loss notices without printing.
A malformed/missing XIIC prefix is rejected, not treated as disconnected. State 0
with 0.0.0.0 activates PPP once, followed by confirmation of state 1 and nonzero IP.



## GMailClient (UNO R4 / ESP32 / ESP32-S3)

Open `examples/UNO_R4/GMailClient/GMailClient.ino`. Replace the local Gmail account,
app password (without display spaces), and recipient placeholders. Two-step verification
and an account permitting app passwords are required. Credentials and Base64 authentication
data are not logged. Do not publish configured credentials.

Install `gts_root_r1.pem` using CertificateManager before running. Download GTS Root R1
from https://i.pki.goog/r1.pem (the prepared file is 1911 bytes). The board's manual
TLS 1.2/server-authentication connection to smtp.gmail.com:465 succeeded using this CA.
PC RSA testing showed smtp.gmail.com -> WR2 -> GTS Root R1; ECDSA testing showed a
different WE2/R4 chain. The modem's negotiated cipher was not measured. Gmail SMTP
login and mail delivery have been confirmed with the automated example by user board testing.
GMailClient calls the same ModemSocket_prepareNetwork used by SSLTCPClient once:
SIM -> APN -> registration -> existing IP check -> activation only when needed.
It defers TLS configuration until time synchronization finishes. ModemTime_beginOnSocket
reuses the socket-owned UART and active PPP link for time commands only; it does not
run ModemTime_prepareNetwork, change UART ownership or activate PPP a second time.
The validated Update To UTC timestamp is written to CCLK and read back before
ModemSocket_configureTls and ModemSocket_open. E0/DST 0 avoids a double timezone offset.
Any time/apply/readback failure stops before TLS/SMTP without retry.
The existing ModemSocket TLS API applies TLS 1.2/server authentication and the standard
SIM/APN/registration/IP preparation. It connects to smtp.gmail.com:465 using implicit TLS.

ModemSmtp validates SMTP numeric/multiline replies, AUTH LOGIN capability, Base64 login,
CRLF normalization, dot stuffing, and the final DATA 250 acceptance. ASCII headers/body only;
the complete message including headers, dot stuffing and terminator must fit 512 bytes.
No attachments, HTML, OAuth or STARTTLS are implemented. Messages run once per reset.
Final DATA response loss means acceptance is unknown; no automatic retry or cleanup follows.
After acceptance, QUIT/cleanup failures never cause a resend. SMTP acceptance does not prove
inbox delivery. Native mock tests, compilation and user board testing are separate verification results.


### Socket SETUP diagnostics

ModemSocket captures bounded modem control lines only while opening a socket. After the
receive pump completes, SETUP RX shows command echo/final/error/asynchronous results,
and SETUP DIAG reports elapsed time, result flags and unresolved receive bytes. A timeout
is distinguished from an explicit modem error. Partial control bytes are shown in HEX.
This trace never captures SMTP authentication or mail payload. Trace truncation does not
change the parser state or trigger a retry. GMailClient labels preparation and socket
connection separately; CERTCHECK success alone is not server certificate validation.


### Explicit UTC clock application

`ModemTime_applySyncedClock()` is an opt-in operation after successful
`ModemTime_syncTime()`. It requires E0/DST 0, writes only a fully validated Update To date
as `AT+CCLK="yy/MM/dd,hh:mm:ss"`, then reads CCLK and requires offset zero (or a missing suffix only in this explicit
E0 sync/write context) plus matching UTC fields within the command elapsed time and a two-second allowance. It runs once per
ModemTime_begin. Invalid/failure/timeout results never supply a fabricated date.
The standalone TimeSync example keeps its existing E9 display and does not call this API.
The Gmail clock-write sequence has been confirmed during user board testing.


### Deferred TLS preparation for GMailClient

Existing ModemSocket_prepareNetwork() calls retain their usual complete TLS preparation.
GMailClient uses ModemSocket_prepareNetwork(true) to defer only TLS settings, then
ModemTime_beginOnSocket(), syncTime(), applySyncedClock(), ModemSocket_configureTls(),
and open(). open() refuses TLS sockets until TLS configuration has succeeded.
ModemSocket_executeControl/executeControlWait are bounded pre-socket transactions used
by ModemTime; only the existing socket parser consumes UART bytes. Standalone TimeSync
retains its own existing AT-based interface and E9 display behavior.


SMTP reply boundaries allow complete empty CRLF lines; other buffered reply bytes
stop the next request with a byte-count diagnostic instead of being discarded.
The normal SMTP path reports leftover byte counts without a raw greeting HEX dump.

An empty reply-boundary CRLF may be split across socket reads. A final CR is retained
as parser state and the next reply read must supply LF; it is not dropped or treated
as a fatal byte by itself. A non-LF continuation still fails. This applies to greeting
and later SMTP reply boundaries without changing resend policy.


ModemSocket enables raw-input buffering during PC diagnostic output. Raw bytes are
queued without interpretation and later consumed in FIFO order by the existing socket
parser. This prevents long SETUP logs from leaving a finite hardware UART FIFO undrained
while the server greeting arrives. The bounded queue overflow is latched and blocks
further socket commands; data is never silently discarded as recovery. This does not
enable raw logging or alter credentials. Other raw clients are not opted in automatically.


GMailClient disables modem command echo with ATE0 and checks its OK before the standard
network preparation. It opts into buffered TLS reception (RECVMODE=0): notification ->
SSLTCPREAD in 32-byte chunks -> complete SMTP reply -> next request. Successful SETUP
and per-chunk AT diagnostics are quiet; SMTP steps/codes remain visible. Optional greeting
wire diagnostics are not enabled in this example. Existing SSLTCPClient keeps its direct
receive and diagnostic defaults. Missing notifications, incomplete READ frames and SMTP
errors still stop without automatic mail retries.

The buffered GMailClient sequence was verified with native mocked modem traffic:
notification-driven 32-byte reads, multiline replies, login, DATA acceptance, QUIT and
IP release. This does not replace reset/repeat tests on the actual modem.


After final DATA 250 acceptance, QUIT may race with the peer closing its TLS socket.
GMailClient distinguishes SMTP 221 from confirmed peer closure without 221. Both can
proceed to IP release, but the latter never claims to have observed 221. Buffered READ
is skipped if close was processed before sending it. A terminal SOCKET ID OPEN FAILED
READ response is classified as that race only with a matching peer-close URC, zero
partial frame/line and no other error/overflow. Without these conditions it remains an
error and cleanup is not guessed. Mail is never retransmitted by this path.

## ESP32 CheckModule

Open `examples/ESP32/CheckModule/CheckModule.ino` and select **ESP32 Dev Module**
(`esp32:esp32:esp32`) in Arduino IDE. Install the complete library, not only the sketch.

- PC serial: `Serial`, 115200 baud.
- Modem serial: `HardwareSerial MDMSerial(2)`, 115200 baud, `SERIAL_8N1`.
- Explicit UART2 pins: RX GPIO16 (from modem TX), TX GPIO17 (to modem RX).
- `ON_OFF`: GPIO18, active HIGH; `LDO_EN`: GPIO5, active HIGH.
- ESP32 host I/O: 3.3 V; use the board's appropriate level interface and common GND.
- Use the same power timing and AT-response shutdown criterion as UNO R4.
- Menu: `h` help, `i` identify, `o` power on and identify, `f` normal power off.
- SIM/network preparation is not required. User testing confirmed boot/identify, manual identify, one off/on cycle and help output.

Use explicit `MDMSerial` UART initialization for ESP32 examples instead of relying
on the core's default `Serial1` or `Serial2` pin mappings.

## ESP32 TransparentBridge

Open `examples/ESP32/TransparentBridge/TransparentBridge.ino` and select
**ESP32 Dev Module** (`esp32:esp32:esp32`). Use the same ESP32 GPIO/UART2 wiring
as CheckModule: ON_OFF GPIO18, LDO_EN GPIO5, RX GPIO16, TX GPIO17.
The modem port is `HardwareSerial MDMSerial(2)` with explicitly assigned pins.
Power timing, raw-mode handoff, 256-byte buffers, pending-write preservation and
bidirectional forwarding follow the UNO R4 example. The PC port remains `Serial`.
After the ready message, enter AT commands at 115200 baud with Carriage Return.
User board testing of ESP32 TransparentBridge is complete.

## ESP32 GenericCommand

Open `examples/ESP32/GenericCommand/GenericCommand.ino` and select **ESP32 Dev Module**.
Use UART2 as `MDMSerial` with RX GPIO16 and TX GPIO17, ON_OFF GPIO18 and LDO_EN GPIO5.
The UNO R4 query sequence, timeouts and error handling are preserved.
AT, CGMM, IVSN, CPIN? and CSQ repeat automatically, waiting 1000 ms after each result.
Use Serial Monitor at 115200 baud; no command input is required.
User board testing of ESP32 GenericCommand is complete.

## ESP32 examples 04–21

The UNO R4 examples NetworkInfo, NetworkConnect, NetworkMonitor, DNS_Check,
FileSystem, FileTransfer, ConfigFile, CertificateManager, TCPClient, UDPClient,
SSLTCPClient, GMailClient, HTTPClient, HTTPSClient, HTTPDownload, MQTTClient,
AWS_IoT and TimeSync have ESP32 counterparts under `examples/ESP32/`.
Select **ESP32 Dev Module** (`esp32:esp32:esp32`). Every example explicitly uses
`HardwareSerial MDMSerial(2)` at 115200 baud, RX GPIO16, TX GPIO17, ON_OFF GPIO18
and LDO_EN GPIO5. Shared protocol, timing and error handling follow the UNO R4 source.

The PC tools and `tools/requirements.txt` are shared between boards. Use the actual
ESP32 COM port and close Serial Monitor before starting a PC tool.
User testing confirmed normal operation of ESP32 examples 04–21. Existing web execution logs remain UNO R4 logs. Compilation and hardware verification are separate checks.

## ESP32-S3 examples 01–21

Open `examples/ESP32S3/<ExampleName>/<ExampleName>.ino`. Select **ESP32S3 Dev Module**
(`esp32:esp32:esp32s3`) in Arduino IDE, with **USB CDC On Boot: Disabled** (board default).
PC `Serial` uses the USB-to-UART connection at 115200 baud. Native USB CDC enabled
configurations are outside this build target because the shared PC logging API
currently accepts `HardwareSerial`.

- Modem: `HardwareSerial MDMSerial(2)`; 115200 baud, `SERIAL_8N1`.
- Explicit UART2 RX GPIO37 from modem TX, TX GPIO38 to modem RX.
- ON_OFF GPIO2 and LDO_EN GPIO1, both active HIGH.
- IOREF: HOST I/O 3.3V; common GND and the appropriate board interface.
- Shared timing, protocol flow and error handling are preserved from ESP32.
- The PC tools and requirements.txt are shared; select the actual USB-UART COM port.

User testing confirmed normal operation of ESP32-S3 examples 01–21. Existing website execution logs remain UNO R4 logs. Compilation and hardware verification are separate checks.

### Boot AT retry policy

`ModemPower_powerOn()` uses the shared `ModemAT_observeAt()` boot check after
one ON_OFF pulse and bounded RX preparation. AT is retried every `atRetryMs`
(default 1000 ms), within `bootTimeoutMs` (default 20000 ms). A timeout without
OK requires at least 10 AT attempts, with one retry interval to receive the
last response even if a shorter boot window is configured. A valid final OK
returns immediately; it does not require ten successful attempts. Each boot
attempt and the total count are logged. RX overflow or an unresolved command
boundary stops the operation early. The ON_OFF pulse is never repeated by this
retry loop. Shutdown observation and already-running quick checks are unchanged.
