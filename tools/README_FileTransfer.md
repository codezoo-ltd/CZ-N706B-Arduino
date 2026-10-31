# FileTransfer interactive guide

Upload the UNO R4 FileTransfer sketch and close Serial Monitor/Serial Plotter.
Use Python 3.8 or later. In the library tools directory, run:

    py -3 -m pip install -r requirements.txt
    py -3 file_transfer.py --port COM35

Replace COM35 with the actual Arduino port. The menu appears after model-response confirmation.
Current default settings: writes 256 bytes, reads 32 bytes, baud 115200. Upload read-back uses
32-byte reads too. A booting board may take up to 30 seconds; running boards respond promptly.
No SIM, APN, XIIC activation, Internet access or FTP server is required.

## Menu at FileTransfer>

- h: help.
- l: modem files and free space.
- u: upload; enter PC source path, modem name, and yes only to replace an existing remote file.
- d: download; enter modem name, PC destination path, and yes only to replace a local file.
- x: list files, enter a deletion target and retype its exact name to confirm.
- q: exit the tool; modem power stays on.

Do not put quotes around paths typed at interactive prompts. The destination directory must
already exist. Remote names use 1-50 ASCII letters, digits, dots, underscores or hyphens.
Directories and external storage are not supported. Uploads accept 1 byte to 1 MiB; downloads
accept 0 bytes to 1 MiB. Declining replacement preserves an existing file. For deletion, an empty
selection or a mismatched confirmation cancels. Only the selected name is deleted.

## Completion and errors

Upload PASS requires the final size and read-back SHA-256 to match the PC source snapshot.
Download PASS means the declared size was received and saved; the printed hash is calculated
from received bytes. Compare it with that run's PC source hash for content identity.
Deletion PASS requires the selected name to be absent from a fresh file list.

Binary data uses declared byte lengths; CR/LF, commas, NUL and OK remain payload. Other services
must not modify the same file during transfer. Same-size concurrent changes are not detected
by download size checks alone. Use the documented default transfer sizes for this setup.

Remote replacement deletes the selected old file before writing. Failed uploads may leave an
incomplete remote file; no automatic retry, cleanup or restoration occurs. Errors end the session.
Inspect the port and remote files before restarting. No new command is sent into a failed transfer.

Downloads write a temporary file in the target directory. Successful receipt and size checks
precede publication. New files use a same-directory hard link without replacing an existing file;
local NTFS is recommended. Unsupported filesystems fail safely. Approved replacement uses
os.replace. Permission errors require checking folder access. Access-denied serial ports require
closing other port users. USB disconnects require checking the cable, device and current COM port.

The UNO R4 sketch only bridges bytes after startup. The PC tool handles the menu and AT commands.
Its supported default mode is write 256 / read 32. Firmware changes are not needed for PC-only
menu/documentation updates. Real 8772-byte download/source hash equality and named-file deletion
have been observed; simulated tests and compilation are separate checks from hardware validation.
