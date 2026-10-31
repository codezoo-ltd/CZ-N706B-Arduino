"""PC file transfer through the UNO R4 FileTransfer byte bridge (Python 3)."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import sys
import tempfile
import time

BAUD_RATE = 115200
QUERY_TIMEOUT_SECONDS = 5
READ_TIMEOUT_SECONDS = 15
WRITE_TIMEOUT_MS = 10000
WRITE_TIMEOUT_SECONDS = WRITE_TIMEOUT_MS / 1000
BOOT_WAIT_SECONDS = 30
SERIAL_POLL_SECONDS = 0.05
WRITE_CHUNK_BYTES = 256
MAX_WRITE_CHUNK_BYTES = 256
READ_CHUNK_BYTES = 32
MAX_READ_CHUNK_BYTES = 256
TX_BURST_BYTES = 32
TX_PAUSE_SECONDS = 0.01
MAX_FILE_BYTES = 1024 * 1024
MAX_LINE_BYTES = 1024
DIAGNOSTIC_TAIL_BYTES = 32
STARTUP_QUERY = "AT+CGMM"
MODEL_PREFIX = b"+CGMM:"


class TransferError(Exception):
    """Stop the session whenever command completion is uncertain."""


def validateName(name):
    # Restrict the PC tool to unambiguous ASCII names in the internal file space.
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,50}", name) or name in (".", ".."):
        raise TransferError("Use 1-50 ASCII letters, digits, underscores, dots or hyphens.")
    return name


class ModemFiles:
    def __init__(self, port, report=print, readChunkBytes=READ_CHUNK_BYTES,
                 writeChunkBytes=WRITE_CHUNK_BYTES):
        if not 1 <= readChunkBytes <= MAX_READ_CHUNK_BYTES:
            raise TransferError("Read chunk size must be between 1 and 256 bytes.")
        if not 1 <= writeChunkBytes <= MAX_WRITE_CHUNK_BYTES:
            raise TransferError("Write chunk size must be between 1 and 256 bytes.")
        self.writeChunkBytes = writeChunkBytes
        self.port = port
        self.report = report
        self.readChunkBytes = readChunkBytes
        self.finalTrace = None
        self.finalByteCount = 0

    def readExact(self, count, deadline, context="RX data"):
        data = bytearray()
        while len(data) < count:
            if time.monotonic() >= deadline:
                raise TransferError(
                    "%s timeout: received %d/%d bytes; no automatic retry."
                    % (context, len(data), count))
            part = self.port.read(count - len(data))
            if part:
                data.extend(part)
                if self.finalTrace is not None:
                    self.finalByteCount += len(part)
                    self.finalTrace.extend(part)
                    del self.finalTrace[:-DIAGNOSTIC_TAIL_BYTES]
        return bytes(data)

    def line(self, deadline):
        data = bytearray()
        while len(data) <= MAX_LINE_BYTES:
            try:
                byte = self.readExact(1, deadline)
            except TransferError as error:
                raise TransferError(
                    "%s; partial control line=%r" % (error, bytes(data[-DIAGNOSTIC_TAIL_BYTES:])))
            if byte == b"\n":
                return bytes(data).rstrip(b"\r")
            data.extend(byte)
        raise TransferError("Response line exceeds the tool limit.")

    def writeExact(self, data, deadline):
        offset = 0
        while offset < len(data):
            if time.monotonic() >= deadline:
                raise TransferError("TX timeout; the remote file may be incomplete.")
            end = min(offset + TX_BURST_BYTES, len(data))
            written = self.port.write(data[offset:end])
            if not written:
                raise TransferError("Serial write made no progress.")
            offset += written
            self.port.flush()
            time.sleep(TX_PAUSE_SECONDS)

    def start(self, command, timeout=QUERY_TIMEOUT_SECONDS):
        self.report("[TX] " + command)
        deadline = time.monotonic() + timeout
        self.writeExact(command.encode("ascii") + b"\r\n", deadline)
        return deadline

    def final(self, deadline, command=None):
        records = []
        while True:
            line = self.line(deadline).strip()
            if not line or (command and line == command.encode("ascii")):
                continue
            if line == b"OK":
                self.report("[RX] OK")
                return records
            if line == b"ERROR" or line.startswith((b"+CME ERROR", b"+CMS ERROR")):
                raise TransferError("Modem rejected command: " + repr(line))
            if line.startswith(b"+FSWF: Timeout"):
                raise TransferError("Modem file-write timeout; remote file may be incomplete.")
            records.append(line)
            self.report("[RX] " + line.decode("ascii", errors="backslashreplace"))

    def query(self, command):
        return self.final(self.start(command), command)

    def connect(self):
        self.report("[INFO] Checking model response; no fixed startup delay for a running bridge.")
        deadline = self.start(STARTUP_QUERY, BOOT_WAIT_SECONDS)
        modelSeen = False
        while True:
            line = self.line(deadline).strip()
            if not line or line == STARTUP_QUERY.encode("ascii"):
                continue
            self.report("[RX startup] " + line.decode("ascii", errors="backslashreplace"))
            if line.startswith(MODEL_PREFIX) and line[len(MODEL_PREFIX):].strip():
                modelSeen = True
            elif line == b"OK" and modelSeen:
                break
            elif line.startswith((b"[ERROR]", b"+CME ERROR", b"+CMS ERROR")) or line == b"ERROR":
                raise TransferError("Bridge/model query failed: " + repr(line))
        self.report("[STATE] FileTransfer connected; no APN or network activation required.")
        self.report("[INFO] Read block: %d bytes; write block: %d bytes."
                    % (self.readChunkBytes, self.writeChunkBytes))

    def listFiles(self):
        records = self.query("AT+FSLIST?")
        files = {}
        for record in records:
            if record.startswith(b"+FSLIST:"):
                record = record[len(b"+FSLIST:"):].strip()
            match = re.fullmatch(rb"([^,]+),(\d+)", record)
            if match:
                name = match[1].decode("ascii", errors="strict")
                if name in files:
                    raise TransferError("Duplicate name in file list.")
                files[name] = int(match[2])
            elif b"," in record or not record.startswith(b"+"):
                raise TransferError("Cannot safely interpret file list: " + repr(record))
        return files

    def numberQuery(self, command, prefix):
        records = self.query(command)
        values = [line[len(prefix):].strip() for line in records if line.startswith(prefix)]
        if len(values) != 1 or not values[0].isdigit():
            raise TransferError("Missing or invalid numeric response.")
        return int(values[0])

    def fileSize(self, name):
        validateName(name)
        return self.numberQuery('AT+FSFS="%s"' % name, b"+FSFS:")

    def freeSpace(self):
        return self.numberQuery("AT+FSLS?", b"+FSLS:")

    def writeChunk(self, name, data, mode):
        command = 'AT+FSWF="%s",%d,%d,%d' % (name, mode, len(data), WRITE_TIMEOUT_MS)
        deadline = self.start(command, WRITE_TIMEOUT_SECONDS)
        # Read the prompt separately: it need not end in CR/LF.
        pending = bytearray()
        while True:
            byte = self.readExact(1, deadline)
            if byte == b">" and not pending.strip():
                break
            pending.extend(byte)
            if byte == b"\n":
                line = bytes(pending).strip()
                pending.clear()
                if line in (b"ERROR", b"OK") or line.startswith(b"+FSWF: Timeout"):
                    raise TransferError("Write prompt not received: " + repr(line))
                if line.startswith((b"+CME ERROR", b"+CMS ERROR")):
                    raise TransferError("Write rejected: " + repr(line))
            if len(pending) > MAX_LINE_BYTES:
                raise TransferError("Write prompt response exceeds the tool limit.")
        self.writeExact(data, deadline)
        self.report("[TX DATA] %d bytes without CR/LF." % len(data))
        self.final(deadline, command)

    def readChunk(self, name, size, position):
        if position == 0:
            command = 'AT+FSRF="%s",0,%d' % (name, size)
        else:
            command = 'AT+FSRF="%s",1,%d,%d' % (name, size, position)
        deadline = self.start(command, READ_TIMEOUT_SECONDS)
        header = bytearray()
        headerBytes = 0
        try:
            while True:
                byte = self.readExact(1, deadline, "File-read header")
                headerBytes += 1
                # CR and LF delimit control lines, never the binary payload.
                if byte in (b"\r", b"\n"):
                    line = bytes(header).strip()
                    header.clear()
                    if not line or line == command.encode("ascii"):
                        continue
                    self.report("[RX control] " + line.decode("ascii", errors="backslashreplace"))
                    if line == b"ERROR" or line.startswith((b"+CME ERROR", b"+CMS ERROR")):
                        raise TransferError("File read rejected: " + repr(line))
                    if line == b"OK":
                        raise TransferError("File payload/header is missing before final OK.")
                    continue
                header.extend(byte)
                match = re.fullmatch(rb"[ \t]*\+FSRF:[ \t]*(\d+),", header)
                if match:
                    returned = int(match[1])
                    if returned != size:
                        raise TransferError(
                            "Unexpected file-read length: expected %d, received %d."
                            % (size, returned))
                    self.report("[RX HEADER] +FSRF: %d bytes; offset %d." % (returned, position))
                    break
                if len(header) > MAX_LINE_BYTES:
                    raise TransferError("Invalid file-read header exceeds the tool limit.")
        except TransferError as error:
            raise TransferError(
                "FSRF header phase: %d control bytes received; buffered prefix %r; %s"
                % (headerBytes, bytes(header[:32]), error)) from error
        # Consume exactly size bytes; do not trim even one space or CR/LF here.
        try:
            data = self.readExact(size, deadline, "File-read payload")
        except TransferError as error:
            raise TransferError("FSRF payload phase: " + str(error)) from error
        self.report("[RX DATA] Received %d/%d file bytes." % (len(data), size))
        self.finalTrace = bytearray()
        self.finalByteCount = 0
        try:
            self.final(deadline)
        except TransferError as error:
            self.report("[RX FINAL DIAG] %d bytes after payload; tail=%r; HEX=%s"
                        % (self.finalByteCount, bytes(self.finalTrace), self.finalTrace.hex(" ")))
            raise TransferError("FSRF final-response phase: " + str(error)) from error
        finally:
            self.finalTrace = None
        return data

    def remoteHash(self, name, size, sink=None):
        digest = hashlib.sha256()
        for position in range(0, size, self.readChunkBytes):
            count = min(self.readChunkBytes, size - position)
            data = self.readChunk(name, count, position)
            digest.update(data)
            if sink is not None:
                sink.write(data)
            self.report("[PROGRESS] Read %d/%d bytes." % (position + count, size))
        return digest.hexdigest()

    def upload(self, local, name, overwrite=False):
        name = validateName(name)
        # Snapshot the source before changing any remote file (maximum 1 MiB).
        with Path(local).open("rb") as source:
            data = source.read(MAX_FILE_BYTES + 1)
        if not data or len(data) > MAX_FILE_BYTES:
            raise TransferError("Upload size must be 1 byte to 1 MiB; no file was changed.")
        expected = hashlib.sha256(data).hexdigest()
        self.report("[SOURCE] %d bytes; SHA-256: %s" % (len(data), expected))
        files = self.listFiles()
        if name in files and not overwrite:
            raise TransferError("Remote file exists. Use --overwrite to replace it explicitly.")
        if self.freeSpace() + files.get(name, 0) < len(data):
            raise TransferError("Insufficient space for the selected upload; old file preserved.")
        if name in files:
            self.query('AT+FSDF="%s"' % name)
            if name in self.listFiles():
                raise TransferError("Remote deletion was not confirmed.")
        if self.freeSpace() < len(data):
            raise TransferError(
                "Insufficient free space; overwrite may already have deleted the old file.")
        for position in range(0, len(data), self.writeChunkBytes):
            block = data[position:position + self.writeChunkBytes]
            self.writeChunk(name, block, 0 if position == 0 else 1)
            self.report("[PROGRESS] Written %d/%d bytes." % (position + len(block), len(data)))
        if self.fileSize(name) != len(data):
            raise TransferError("Uploaded file size mismatch.")
        actual = self.remoteHash(name, len(data))
        self.report("[VERIFY] Source SHA-256: " + expected)
        self.report("[VERIFY] Read-back SHA-256: " + actual)
        if actual != expected:
            raise TransferError(
                "Uploaded content hash mismatch; remote file remains for inspection.")
        self.report("[PASS] Upload verified by size and read-back SHA-256: " + actual)

    def delete(self, name=None, ask=input):
        files = self.listFiles()
        self.report("[FILES] Modem internal files:")
        for fileName, size in sorted(files.items()):
            self.report("  %s (%d bytes)" % (fileName, size))
        if not files:
            self.report("[INFO] File list is empty; nothing was deleted.")
            return False
        if name is None:
            name = ask("File name to delete (Enter to cancel): ").strip()
            if not name:
                self.report("[CANCELLED] No file was deleted.")
                return False
        name = validateName(name)
        if name not in files:
            raise TransferError("Selected file is not in the modem file list.")
        self.report("[DELETE] Selected %s (%d bytes). This cannot be undone."
                    % (name, files[name]))
        confirmation = ask("Confirm deletion by typing the exact file name: ")
        if confirmation != name:
            self.report("[CANCELLED] File name did not match; nothing was deleted.")
            return False
        self.query('AT+FSDF="%s"' % name)
        if name in self.listFiles():
            raise TransferError("Deletion not confirmed; selected file is still listed.")
        self.report("[PASS] Deleted %s; absence confirmed by file list." % name)
        return True

    def download(self, name, local, overwrite=False):
        name = validateName(name)
        target = Path(local)
        if target.exists() and not overwrite:
            raise TransferError("Local file exists. Use --overwrite to replace it explicitly.")
        size = self.fileSize(name)
        if size < 0 or size > MAX_FILE_BYTES:
            raise TransferError("Remote file exceeds the 1 MiB tool limit.")
        fd, temp = tempfile.mkstemp(prefix=target.name + ".", suffix=".part", dir=target.parent)
        try:
            with os.fdopen(fd, "wb") as output:
                digest = self.remoteHash(name, size, output)
            if self.fileSize(name) != size:
                raise TransferError("Remote file size changed during download.")
            if overwrite:
                os.replace(temp, target)
            else:
                # Same-directory hard link publishes atomically without replacing an existing file.
                os.link(temp, target)
                os.unlink(temp)
            self.report("[PASS] Downloaded %d bytes; SHA-256: %s" % (size, digest))
        except BaseException:
            if os.path.exists(temp):
                os.unlink(temp)
            raise


def showHelp():
    print("h help | l list files/free space | u upload | d download | x delete | q quit")
    print("Uploads read back the file and compare SHA-256. No automatic retry or cleanup.")
    print("Overwrite deletes/replaces the selected old file; it cannot be undone by this tool.")


def interactive(modem):
    showHelp()
    while True:
        key = input("FileTransfer> ").strip().lower()
        if key == "q":
            return
        if key == "h":
            showHelp()
        elif key == "l":
            print(modem.listFiles())
            print("[FREE BYTES]", modem.freeSpace())
        elif key == "x":
            modem.delete()
        elif key == "u":
            local = input("PC source path: ").strip()
            name = input("Modem file name: ").strip()
            replace = input("Replace existing remote file? Type yes: ").strip() == "yes"
            modem.upload(local, name, replace)
        elif key == "d":
            name = input("Modem file name: ").strip()
            local = input("PC destination path: ").strip()
            replace = input("Replace existing local file? Type yes: ").strip() == "yes"
            modem.download(name, local, replace)
        else:
            print("[INFO] Unknown key; enter h for help.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Arduino port, for example COM7")
    parser.add_argument("--read-chunk-bytes", type=int, default=READ_CHUNK_BYTES,
                        help="FSRF block size, 1-256 bytes; default 32 (content verified)")
    parser.add_argument("--write-chunk-bytes", type=int, default=WRITE_CHUNK_BYTES,
                        help="FSWF block size, 1-256 bytes; default 256")
    commands = parser.add_subparsers(dest="action")
    commands.add_parser("list")
    delete = commands.add_parser("delete", help="List files and confirm one file deletion")
    delete.add_argument("name", nargs="?", help="Remote name; omit to choose after listing")
    upload = commands.add_parser("upload")
    upload.add_argument("local")
    upload.add_argument("--name", required=True)
    upload.add_argument("--overwrite", action="store_true")
    download = commands.add_parser("download")
    download.add_argument("name")
    download.add_argument("local")
    download.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.read_chunk_bytes <= MAX_READ_CHUNK_BYTES:
        parser.error("--read-chunk-bytes must be between 1 and 256")
    if not 1 <= args.write_chunk_bytes <= MAX_WRITE_CHUNK_BYTES:
        parser.error("--write-chunk-bytes must be between 1 and 256")
    try:
        import serial
    except ImportError:
        parser.error("Install pyserial with: py -3 -m pip install pyserial")
    try:
        with serial.Serial(args.port, BAUD_RATE, timeout=SERIAL_POLL_SECONDS,
                           write_timeout=QUERY_TIMEOUT_SECONDS) as port:
            modem = ModemFiles(port, readChunkBytes=args.read_chunk_bytes,
                               writeChunkBytes=args.write_chunk_bytes)
            modem.connect()
            if args.action == "list":
                print(modem.listFiles())
                print("[FREE BYTES]", modem.freeSpace())
            elif args.action == "delete":
                modem.delete(args.name)
            elif args.action == "upload":
                modem.upload(args.local, args.name, args.overwrite)
            elif args.action == "download":
                modem.download(args.name, args.local, args.overwrite)
            else:
                interactive(modem)
    except KeyboardInterrupt:
        print("[CANCELLED] Interrupted by user; inspect remote files before restarting.",
              file=sys.stderr)
        return 130
    except (TransferError, OSError, UnicodeError, serial.SerialException) as error:
        print("[FAIL] Session stopped: %s" % error, file=sys.stderr)
        print("[INFO] Close Serial Monitor/other port users; verify the current COM port.",
              file=sys.stderr)
        print("[INFO] USB disconnects require inspection; no automatic retry or file deletion.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
