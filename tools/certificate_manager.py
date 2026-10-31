"""Interactive certificate manager through the CertificateManager byte bridge."""
import argparse
from pathlib import Path
import re
import sys
import time

# Reuse only the tested serial transport; no general filesystem commands are used.
from file_transfer import ModemFiles, TransferError, validateName
from file_transfer import BAUD_RATE, SERIAL_POLL_SECONDS, QUERY_TIMEOUT_SECONDS
from file_transfer import BOOT_WAIT_SECONDS, TX_BURST_BYTES, TX_PAUSE_SECONDS

MAX_CERT_BYTES = 64 * 1024  # PC tool limit, not a claimed modem certificate limit.
OPTIONAL_FINAL_QUIET_SECONDS = 0.15
OPTIONAL_FINAL_LIMIT_SECONDS = 1.0
STARTUP_INITIAL_WAIT_SECONDS = 1.0
STARTUP_QUIET_SECONDS = 0.25
STARTUP_LINE_BYTES = 1024


class Certificates:
    def __init__(self, port, report=print):
        self.port = port
        self.report = report
        self.transport = ModemFiles(port, report=report)
        self.optionalFinal = False

    @staticmethod
    def error(line):
        return line == b"ERROR" or line.startswith(
            (b"+CME ERROR", b"+CMS ERROR", b"+CERTADD: ERROR", b"+CERTCHECK: ERROR"))

    def finishOptionalFinal(self):
        # CERTADD/CERTCHECK named responses can complete without a separate OK.
        # Consume only blank lines, optional OK and clearly labeled URCs.
        if not self.optionalFinal:
            return
        deadline = time.monotonic() + OPTIONAL_FINAL_LIMIT_SECONDS
        quiet = time.monotonic() + OPTIONAL_FINAL_QUIET_SECONDS
        pending = bytearray()
        while time.monotonic() < deadline:
            part = self.port.read(1)
            if not part:
                if time.monotonic() >= quiet:
                    if pending.strip():
                        raise TransferError("Partial response after certificate completion.")
                    self.optionalFinal = False
                    return
                continue
            quiet = time.monotonic() + OPTIONAL_FINAL_QUIET_SECONDS
            pending.extend(part)
            if len(pending) > 1024:
                raise TransferError("Trailing certificate response exceeds the tool limit.")
            if part == b"\n":
                line = bytes(pending).strip()
                pending.clear()
                if not line:
                    continue
                if line == b"OK":
                    self.report("[RX optional] OK")
                elif line.startswith(b"+") and not self.error(line):
                    self.report("[URC] " + line.decode("ascii", errors="backslashreplace"))
                else:
                    raise TransferError("Unexpected trailing response: " + repr(line))
        raise TransferError("Certificate response did not reach a quiet boundary.")

    def start(self, command, timeout=QUERY_TIMEOUT_SECONDS):
        self.finishOptionalFinal()
        return self.transport.start(command, timeout)

    def nextLine(self, deadline, command, hideUnrecognized=False):
        while True:
            line = self.transport.line(deadline).strip()
            if not line or line == command.encode("ascii"):
                continue
            known = line in (b"OK", b"CONNECT") or line.startswith(
                (b"+CERTADD:", b"+CERTCHECK:", b"+CME ERROR", b"+CMS ERROR")) or self.error(line)
            if hideUnrecognized and not known:
                self.report("[RX] Non-control data omitted from certificate diagnostics.")
            else:
                self.report("[RX] " + line.decode("ascii", errors="backslashreplace"))
            if self.error(line):
                raise TransferError("Certificate command failed: " + repr(line))
            return line

    def waitCertificateToken(self, deadline, command, expected, phase):
        # Certificate control tokens may not be terminated by CR/LF.
        # Recognize complete tokens by grammar instead of waiting for newline.
        pending = bytearray()
        while True:
            try:
                byte = self.transport.readExact(1, deadline, phase)
            except TransferError as error:
                raise TransferError("%s did not complete; buffered control prefix=%r; %s"
                                    % (phase, bytes(pending[:64]), error)) from error
            if byte in (b"\r", b"\n"):
                line = bytes(pending).strip()
                pending.clear()
                if not line or line == command.encode("ascii"):
                    continue
                if self.error(line):
                    raise TransferError("Certificate command failed: " + repr(line))
                if line == b"OK" or line.startswith((b"+CERTADD:", b"+CERTCHECK:")):
                    raise TransferError("Unexpected certificate response during %s: %r" % (phase, line))
                self.report("[RX] Non-terminal control/URC line omitted.")
                continue
            pending.extend(byte)
            if len(pending) > 1024:
                raise TransferError("Certificate control response exceeds the tool limit.")
            token = bytes(pending).strip()
            if expected.fullmatch(token):
                self.report("[RX] " + token.decode("ascii"))
                return token
            if token in (b"ERROR", b"+CERTADD: ERROR", b"+CERTCHECK: ERROR"):
                raise TransferError("Certificate command failed: " + repr(token))

    def waitStartupBoundary(self, deadline, beforeQueries):
        # Consume and report startup lines; never discard an incomplete line.
        start = time.monotonic()
        lastRx = start
        pending = bytearray()
        bootSeen = False
        bridgeReady = False
        bootPrefixes = (b"ESP-ROM:", b"ets ", b"Build:", b"rst:", b"[STATE]",
                        b"[TX power window]", b"[RX echo]", b"[RX final]")
        while time.monotonic() < deadline:
            part = self.port.read(1)
            if part:
                lastRx = time.monotonic()
                pending.extend(part)
                if len(pending) > STARTUP_LINE_BYTES:
                    raise TransferError("Startup line exceeds the tool limit.")
                if part != b"\n":
                    continue
                line = bytes(pending).strip()
                pending.clear()
                if not line:
                    continue
                self.report("[RX startup] " + line.decode("ascii", errors="backslashreplace"))
                if self.error(line) or line.startswith(b"[ERROR]"):
                    raise TransferError("Bridge startup failed: " + repr(line))
                ready = line == b"[READY] CertificateManager bridge ready."
                boot = line.startswith(bootPrefixes)
                if beforeQueries:
                    bootSeen = bootSeen or boot
                    bridgeReady = bridgeReady or ready
                elif (ready or boot or not line.startswith(b"+")
                      or line.startswith((b"+CGMM:", b"+CERTADD:", b"+CERTCHECK:"))):
                    raise TransferError("Unexpected startup response after model confirmation: " + repr(line))
                continue
            now = time.monotonic()
            minimumWait = STARTUP_INITIAL_WAIT_SECONDS if beforeQueries else 0
            if (now - start >= minimumWait and now - lastRx >= STARTUP_QUIET_SECONDS
                    and (not bootSeen or bridgeReady)):
                if pending.strip():
                    raise TransferError("Incomplete startup line: " + repr(bytes(pending)))
                return
        raise TransferError("Startup boundary timeout; no menu or certificate operation started.")

    def connect(self):
        deadline = time.monotonic() + BOOT_WAIT_SECONDS
        self.report("[STATE] Waiting for bridge startup output to finish.")
        self.waitStartupBoundary(deadline, beforeQueries=True)
        # One outstanding command only. Readiness URCs never trigger another TX.
        for command in ("AT", "AT+CGMM"):
            self.report("[TX] " + command)
            self.transport.writeExact(command.encode("ascii") + b"\r\n", deadline)
            modelSeen = False
            while True:
                line = self.nextLine(deadline, command)
                if line.startswith(b"[ERROR]") or line == b"[READY] CertificateManager bridge ready.":
                    raise TransferError("Bridge restarted during startup query: " + repr(line))
                if line == b"+PBREADY":
                    self.report("[URC] Modem ready; waiting for the current command to finish.")
                    continue
                if line.startswith(b"+CGMM:") and line[len(b"+CGMM:"):].strip():
                    if command != "AT+CGMM":
                        raise TransferError("Unexpected model response during AT readiness check.")
                    modelSeen = True
                elif line == b"OK":
                    if command == "AT" or modelSeen:
                        break
                    raise TransferError("Model query ended without a model response.")
        self.waitStartupBoundary(deadline, beforeQueries=False)
        self.report("[STATE] CertificateManager connected; SIM/network not required.")
        self.report("[INFO] DTR enabled; certificate bytes are sent without conversion.")

    def listCertificates(self):
        command = "AT+CERTCHECK?"
        deadline = self.start(command)
        names = []
        while True:
            line = self.nextLine(deadline, command)
            if line == b"OK":
                return names
            if line.startswith(b"+"):
                self.report("[URC] Non-list notification retained in diagnostics.")
                continue
            name = line.decode("ascii", errors="strict")
            if name.startswith('"') and name.endswith('"'):
                name = name[1:-1]
            validateName(name)
            if name in names:
                raise TransferError("Duplicate name in certificate list.")
            names.append(name)

    def showList(self):
        names = self.listCertificates()
        self.report("========== MODEM CERTIFICATES ==========")
        for name in names:
            self.report("  " + name)
        if not names:
            self.report("  (empty)")
        self.report("=======================================")
        return names

    def checkCertificate(self, name):
        name = validateName(name)
        command = 'AT+CERTCHECK="%s"' % name
        deadline = self.start(command)
        expected = re.compile(rb'\+CERTCHECK:\s*' + re.escape(name.encode("ascii")) + rb',\s*OK')
        self.waitCertificateToken(deadline, command, expected, "CERTCHECK confirmation")
        self.optionalFinal = True
        self.report("[VERIFY PASS] CERTCHECK confirmed: " + name)

    def upload(self, local, name, ask=input):
        name = validateName(name)
        with Path(local).open("rb") as source:
            data = source.read(MAX_CERT_BYTES + 1)
        if not data or len(data) > MAX_CERT_BYTES:
            raise TransferError("PC tool accepts certificate files from 1 byte to 64 KiB.")
        self.report("[SOURCE] " + str(Path(local).resolve()))
        self.report("[TARGET] " + name)
        self.report("[SIZE] %d bytes; original CR/LF preserved." % len(data))
        if name in self.listCertificates():
            self.report("[REPLACE] Same-name certificate exists; registration may overwrite it.")
            if ask("Replace it? Type yes (Enter to cancel): ").strip() != "yes":
                self.report("[CANCELLED] Existing certificate unchanged.")
                return False
        elif ask("Register certificate? Type yes (Enter to cancel): ").strip() != "yes":
            self.report("[CANCELLED] No certificate was registered.")
            return False
        command = 'AT+CERTADD="%s",%d' % (name, len(data))
        deadline = self.start(command, 15)
        self.waitCertificateToken(deadline, command, re.compile(rb"CONNECT"),
                                  "CERTADD CONNECT prompt")
        bursts = (len(data) + TX_BURST_BYTES - 1) // TX_BURST_BYTES
        duration = max(30, 15 + 2 * (len(data) * 10 / BAUD_RATE + bursts * TX_PAUSE_SECONDS))
        deadline = time.monotonic() + duration
        self.transport.writeExact(data, deadline)
        self.report("[TX DATA] %d certificate bytes sent; no extra CR/LF." % len(data))
        expected = re.compile(rb'(?:\+CERTADD:\s*(\d+),\s*OK|OK)')
        token = self.waitCertificateToken(deadline, command, expected, "CERTADD write completion")
        match = expected.fullmatch(token)
        if match[1] is not None and int(match[1]) != len(data):
            raise TransferError("CERTADD acknowledged a different byte count.")
        self.optionalFinal = True
        self.report("[REGISTER PASS] Certificate write OK; checking the stored name.")
        self.checkCertificate(name)
        self.report("[PASS] Certificate registered and confirmed: " + name)
        return True

    def delete(self, ask=input):
        names = self.showList()
        if not names:
            self.report("[INFO] Nothing to delete.")
            return False
        name = ask("Certificate name to delete (Enter to cancel): ").strip()
        if not name:
            self.report("[CANCELLED] Nothing deleted.")
            return False
        name = validateName(name)
        if name not in names:
            raise TransferError("Selected name is not in the certificate list.")
        if ask("Confirm deletion by typing the exact certificate name: ") != name:
            self.report("[CANCELLED] Name did not match; nothing deleted.")
            return False
        command = 'AT+CERTDEL="%s"' % name
        deadline = self.start(command)
        while self.nextLine(deadline, command) != b"OK":
            pass
        if name in self.listCertificates():
            raise TransferError("Certificate is still listed after deletion.")
        self.report("[DELETE PASS] Deleted certificate: " + name)
        return True


def showHelp():
    print("h help | i certificate list | u upload certificate | x delete certificate | q quit")
    print("u: PC source path -> modem certificate name -> byte count -> CONNECT -> data -> CERTCHECK.")
    print("x: select one listed name and retype it to confirm. There is no delete-all action.")
    print("Registration/check success does not prove TLS connectivity or certificate trust.")


def interactive(modem, ask=input):
    showHelp()
    while True:
        key = ask("CertificateManager> ").strip().lower()
        if key == "q":
            modem.finishOptionalFinal()
            print("[STATE] Session complete; certificates remain stored.")
            return
        if key == "h":
            showHelp()
        elif key == "i":
            modem.showList()
        elif key == "u":
            local = ask("PC certificate path: ").strip().strip('"')
            name = ask("Modem certificate name: ").strip()
            modem.upload(local, name, ask=ask)
        elif key == "x":
            modem.delete(ask=ask)
        else:
            print("[INFO] Unknown key; enter h for help.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Arduino port, for example COM7")
    args = parser.parse_args()
    try:
        import serial
    except ImportError:
        parser.error("Install pyserial: py -3 -m pip install pyserial")
    try:
        port = serial.Serial(port=None, baudrate=BAUD_RATE, timeout=SERIAL_POLL_SECONDS,
                             write_timeout=QUERY_TIMEOUT_SECONDS)
        port.port = args.port
        port.dtr = True
        with port:
            modem = Certificates(port)
            modem.connect()
            interactive(modem)
    except KeyboardInterrupt:
        print("[CANCELLED] Interrupted; inspect modem certificate state before restarting.", file=sys.stderr)
        return 130
    except (TransferError, OSError, UnicodeError, serial.SerialException) as error:
        print("[FAIL] Session stopped: " + str(error), file=sys.stderr)
        print("[INFO] No automatic retry or deletion. Close other programs using the COM port.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
