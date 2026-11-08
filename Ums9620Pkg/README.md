# UMS9620 inherited-LK diagnostic port

## Controls candidate (2026-11-05)

`Ump9620ResetSystemLib` adds bounded native PMIC shutdown during Boot Services:
clear RTC interrupts, unlock protected power control, request power-off. It
excludes concurrent ADI users and returns on failure; OS runtime reset retains
PSCI. The sequence is from the pinned LK qogirn6pro/UMP9620 register definitions.
Native shutdown is not yet hardware accepted; black-screen/backlight feedback
from the short RAM session remains unresolved.

`Charger.c` keeps both channels stopped and preserves user intent while an
otherwise validated main charger is waiting for power-good or source detection.
Faults, OTG and changed profiles still reject charging. A cached register decision
is periodically logged without extra I2C traffic. Actual replug testing is pending.
Recovery actions and application-log storage belong to the independent runtime.

## Android boot status (2026-11-05)

[AndroidLk](Applications/AndroidLk/README.md) consumes the suspended-LK return
contract, exits UEFI and resumes the native Android loader without resetting.
The direct3 RAM diagnostic completed Android A startup with ADB/root and
file-based encryption confirmed. The paired Android EFI is installed on the ESP.
The matching 5 MiB resident image (SHA256 `072bcff01b32283fc22405ddc673244ec67f302d93b9674ad714dba3575a867d`)
has been flashed to physical B with full readback. Both RAM and resident normal
menu paths now load the ESP application and complete Android A startup, with
ADB/root, `boot_completed=1` and file-based encryption confirmed. The user also
accepted physical-key selection and Android startup. This covers an ordinary
reboot from B; a full removal of power has not been tested.

The previous return to the menu was `EFI_ACCESS_DENIED`: diagnostic BDS omitted
EndOfDxe, so upstream SecurityStubDxe deferred third-party ESP applications.
BDS now signals EndOfDxe before connecting controllers and launching the menu.
UEFI Fastboot remains unimplemented; the existing USB debugger belongs to LK.

The older [AndroidBoot prototype](Applications/AndroidBoot/README.md) loads the
raw Android kernel and a captured DT. Its staging/CRC preflight passed, and the
corrected ARM64 handoff reached the kernel, but userdata key decryption failed
and init requested recovery. It remains a separate experiment. The failed reset
bridge has been removed. Linux EFI remains independent and incomplete.

References: [Android boot headers](https://source.android.com/docs/core/architecture/bootloader/boot-image-header)
and the [ARM64 boot contract](https://www.kernel.org/doc/html/latest/arch/arm64/booting.html).

This is a new adaptation on tianocore/edk2 e7aac7fc, not the withdrawn W202Pkg. Platform constants are traced to sprd-linux/lk 47f4d044 (ums9620_2h10/qogirn6pro); the public tree is not an exact W202DS board source.

The platform contains no PocketJS pages or engine. `w202ds-boot` supplies a separately linked EFI image at packaging time. The public `pocketjs_boot_context.h` is a declared build input selected by the caller through `PJB_BOOT_ABI_INCLUDE`; this checkout contains no link or relative path into the PocketJS source tree.

## Mandatory entry contract and RAMBoot evidence

- A preceding loader initializes DDR, clocks, power, panel and continuous scanout. The matching W202DS HX83102J setup is absent from public LK.
- UEFI reservation: 0x82800000..0x85ffffff (56 MiB), with a 2 MiB FD at the bottom and working RAM at 0x83000000..0x85ffffff. This avoids the reference CHIPRAM environment at 0x82000000 and the LK download buffer at 0x86000000. It has not yet been verified against all live LK allocations. The loader must locate and enter the rebased SEC image: the bare FD is not a drop-in uboot image.
- Enter with primary MPIDR 0 only, AArch64 EL1/EL2 as required by ArmPlatform PrePi, interrupts masked, MMU/caches in the documented upstream SEC entry state; other CPUs quiescent. No SMP bringup.
- Keep framebuffer 0xb0000000, 1200×1920 RGB565, stride 2400 bytes alive. GOP uses an uncached map and does not call fixed-address original LK functions. Native r6p0 run/layer-latch, bounded raw-status polling and cache cleaning are now implemented; inherited register geometry/format are checked before use.
- GIC and architectural timer addresses/IRQs follow the LK SoC reference; the timer has delivered input polling during real SEC/DXE/PocketJS runs.

The candidate has inherited GOP, core UEFI protocols, emulated volatile variables
and a diagnostic BDS entry. BoardServicesDxe provides native FGU telemetry,
bounded NTC ADC conversions, dual BQ2560x status reads and retained-firmware HX83102J touch through EFI
Absolute Pointer. It requires a validated LK handoff record. The RAMBoot shim
captures stock voltage and chip-calibrated NTC coefficients before cleanup;
EDK2 never calls into LK. Handoff v2 is 48 bytes; v1 remains accepted for FGU.
Two separate reserved RAM pages hold the record at 0x82ffe000 and trace at
0x82fff000; they are no longer stored in the 2 MiB FD. Legacy 8 MiB images
included these pages in their erased tail. The loader verifies the image CRC,
initializes both pages independently and PEI reserves them before DXE.

The board composition enables `W202_TOUCH_RELOAD=TRUE` and supplies
`W202_TOUCH_FIRMWARE` explicitly. The EDK2 checkout itself has no dependency on
another project's firmware directory. The pinned guoxian image is 261120 bytes,
SHA256 `bdcd327d48628dd703b1d0a8ea778ef7f7aacd7beb227efdb78ea788928f0156`.
Before enabling input, the loader validates the exact partition map and software
CRCs, stops the touch MCU, uploads entity/configuration SRAM through bounded SPI
bursts, checks hardware CRCs, and activates normal reports. It runs once per UEFI
boot, never from the IRQ callback or a periodic input poll. A failed upload does
not install the pointer protocol. Explicit `W202_TOUCH_RELOAD=FALSE` retains the
older inherited-image path for comparison. No GPIO/TCON reset register is written;
this does not imply all shared-IC power states have been validated.

On 2026-11-05 the inherited-image baseline produced 32 empty IRQ reports. Upload
completed with entity CRC 0, config CRC 5ced9ac0, reload 72c0 and run state 5.
A 180-second RAM run recorded 21 IRQ callbacks, all 21 valid and no empty report;
the user confirmed working input and a normal display. The 5 MiB resident image
was then written/read back on physical B, and the user accepted touch before and
after a forced reboot. Complete removal of power and long-duration use remain
untested. `HX_UPLOAD` exposes stage/CRCs; stage 7 alone is not proof of input.
Host tests cover the real firmware CRC/map, exact upload data, SPI/FIFO boundaries
and bounded errors. The standalone `tests/touch_reload.c` takes the firmware path
as its argument; `tests/test_hardware.py` includes the SPI burst regression.
 Touch is suspended
before StartImage, resumed when a child returns, and stopped permanently at
ExitBootServices. Power fields fail independently. Voltage, signed current,
external input, NTC temperature and actual charging status have r36 RAMBoot
evidence. Capacity uses retained Android UI SOC plus subsequent coulomb-counter
change with the stock 7286 mAh default; it is explicitly marked estimated.
Learned capacity, time spent outside EDK2 and Android temperature/SOC filters
are not reconstructed. Since r37, the driver enables only the main charger's
CHG_CONFIG after exact retained-profile, battery-presence, temperature, voltage
and fault checks. The optional ChargeControlProtocol defaults to stop on both chips. Explicit
enable requests activate only the main path; every second the driver checks the
same limits and latches the request off on failure. Both chips are stopped and
read back, including inherited enables. Quiesce must confirm charging has
stopped before handing control to a child. The inherited profile disables the
charger watchdog; no hang-safe charging supervisor or battery bypass is provided.
ADC/I2C state is restored after each bounded read; failed restoration stops
further use of the affected peripheral. Charging=false requires valid status
from both chips; charging=true can be established by either active chip.

Native keys expose SimpleTextIn using inherited analogue EIC/GPIO masks without
changing pinmux or PMIC configuration. Volume-up uses EIC164 (high), volume-down
GPIO7 (low), and r70 adds power on EIC161 (low) as ScanCode=0/UnicodeChar=13
(Enter). Entry holds are ignored; a 30 ms debounce produces one event per press.
The RAM loader now restores the missing EIC164 input mask before installing keys;
physical volume selection and power-key confirmation passed user acceptance.
Keys use a debounce timer;
touch SPI reads are triggered by EIC interrupt events, with no periodic touch
poll. Both share the quiesce/exit lifecycle. Native reset and persistent UEFI variables remain incomplete. AndroidLk has
RAM and resident Android evidence above; the Linux loader remains unfinished. Application settings use the ESP
through the independently linked PocketJS host. Real RAMBoot has reached SEC/DXE/PocketJS,
read calibrated FGU voltage, delivered keys/pointer events, and exercised a
returning EFI child. The current reload build has valid IRQ packet and user acceptance evidence
above; earlier empty-packet and polling experiments are historical.
The optional HandoffProbe is excluded unless W202_HANDOFF_TEST=TRUE. The optional
colour stripe/hash diagnostics are excluded unless W202_DISPLAY_TEST=TRUE.
W202_TOUCH_IRQ_DIAG=TRUE enables an isolated, logged touch sample on each new
debounced key press. It never synthesizes pointer input and is disabled
by default. The r47 experiment uses this to separate report availability from
touch-IRQ timing.
That r47 comparison still returned empty reports with a held finger; IRQ
delivery alone therefore does not explain the missing coordinates.
W202_TOUCH_RESUME_TEST=TRUE adds a bounded AP sleep/awake experiment, always
attempting awake on a failed sleep. It is disabled by default. Diagnostic code
samples match seven stock firmware words, not a full-image integrity check.
W202_TOUCH_TEST=TRUE (the default) includes the standalone TouchScope EFI app.
W202_TOUCH_DIRECT=TRUE selects it directly in diagnostic BDS; the default is
FALSE so normal boots enter the PocketJS menu. Volume down clears once and
returns to the menu on a second press within 800 ms.
Its green pulse submits a small rectangle every 20 ms from the application
loop; volume up toggles this with a static blue square, and volume down clears
the drawing. Touch GetState still consumes only the IRQ-fed queue. No GOP
timer or periodic touch SPI sampling is added by this display comparison.
The r51 device comparison produced no valid reports in either user-observed
mode. Frequent display submissions did not restore touch. The r53 menu contains
android, DebianServer, DebianKDE and UEFITEST-Touch. AndroidLk now provides the Android EFI entry; normal menu acceptance and the
Linux loaders remain pending.

EmmcDxe uses the inherited SPRD SDHCI-r11 controller at 0x22200000.
LK initializes the card, clock, DLL, voltage and bus timing. The native UEFI
adapter follows LK's working SDMA transfer sequence with an aligned private
bounce page and explicit CPU cache maintenance. The former PIO path timed out
waiting for CMD8 buffer-ready on hardware. No LK function is called after handoff.

BlockIo supports sector reads and writes, including unaligned caller buffers.
Writes are limited to the one ESP whose bounds agree in both CRC-valid GPT
copies; overlapping partitions, ambiguous ESPs and damaged tables cannot enable
writes. GPT, A/B boot partitions and all other ranges remain write-protected.
The card's cache setting is inherited: disabled cache waits for data completion;
enabled cache additionally uses FLUSH_CACHE and checks the following EXT_CSD/R1.
The W202DS test card has cache disabled, so the enabled-cache path has model
coverage only. Transfers restore controller registers, stop service after an
error and invalidate BlockIo at ExitBootServices.

DiskIo, PartitionDxe and EnhancedFatDxe expose the standard filesystem protocol.
The 2026-11-04 RAM tests read EFI/W202DS/README.TXT and wrote/read an 8193-byte
cross-cluster file, then verified it after watchdog reset and a second RAMBoot.
This does not install an Android/Linux EFI loader. Diagnostic BDS normally only
inspects files. W202_STORAGE_TEST=TRUE enables the explicit UEFI-RW.BIN test,
omits BoardServicesDxe and stops before the UI to preserve retained logs for the
recovery watchdog. Never enable that option in a normal resident image.
W202_BOARD_SERVICES=FALSE separately permits isolated application storage tests.
The outer build script exposes these options as environment inputs; defaults
are FALSE and TRUE respectively. Native MMIO model tests cover DMA bytes,
write boundaries, cache handling, failures, register restoration and lifecycle;
GPT tests cover both tables, CRCs, ambiguous/overlapping ranges and I/O failures.

The offline r54 TouchScope candidate waits on the key, pointer and timer events
with WaitForEvent. Only the selected input event reads its protocol. The 20 ms
pulse timer never reads input or SPI; static mode uses a ten-second log timer.
Release events draw nothing and end the stroke; invalid coordinates or input
errors break the gesture. Stale input events return to the wait. Normal exit
returns success, cancels/closes its timer, frees its pixel buffer and quiesces
the platform, retaining any earlier error. Thirteen host scenarios exercise the
actual application C and pixel output. The linked r54 FV contains this new
standalone application and the four-entry menu. It has not been run on the
tablet; the hardware's empty-report problem remains open.

RAMBoot defaults to a 30-second recovery watchdog; a separately built loader can
select a bounded 30–180 second observation window with
EXTRA_CFLAGS=-DW202_RECOVERY_WATCHDOG_MS=180000. The r56 resident flags its handoff
so BoardServicesDxe renews the already running PMIC reset watchdog every 1000
active key-timer callbacks (nominally ten seconds), loading 180 seconds each time.
The native watchdog control and clocks are not changed. A failed renewal latches
a fault; quiesce and ExitBootServices stop renewal, leaving bounded recovery.
This is firmware liveness recovery, not a charger safety supervisor.

The default FD is now 2 MiB. The Android wrapper removes only an erased 0xff
suffix and restores it in RAM, with full logical and stored CRC checks. This
adds no compression codec; existing EDK2 compressed firmware volumes remain.
The thin LK bootstrap and compact resident fit in the B-slot 5 MiB uboot
container. Only that packaged container can be installed; neither the raw FD,
the Android wrapper nor the PLK1 download bundle is a flashable uboot image.
Permanent boot validation and limitations belong to the board hardware report.

Build artifacts and logs belong to the caller's build directory. Hardware evidence belongs to the W202DS research workspace.
