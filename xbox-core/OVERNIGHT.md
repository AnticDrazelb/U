# Overnight work (2026-11-09 to 10)

The owner can't test tonight and asked for everything feasible on every part of the emulator. An
hourly scheduled check-in resumes the session (it survives usage-limit resets); each one takes the
first unfinished item below, finishes it (code, tests, the regression set both normally and with
`XB_TEST_FS_AS_PS4=1`, the FreeBSD VM tests), commits, pushes, rebuilds the packages, and records
what was done and verified here. The scheduled task is removed in the morning (after 14:00 UTC).

Order: what Halo will meet next after the network adapter, then everything else by how likely a
retail game is to need it. Each item says how it is checked without the game.

## Items

1. **The second USB controller (0xfed08000, IRQ 9).** PCI reports both, as every console has, but
   only the first is emulated: the XDK's USB driver sets up both and would wait on the second.
   Check: the usb sample and the kernel test still pass; a test that resets the second controller.
2. **USB as the XDK's driver uses it:** audit `usb.cpp` against the OHCI 1.0a register set (every
   register, the root hub's port bits, the done queue, frame number, interrupts) and fix gaps.
3. **The APU and DirectSound start-up:** what DirectSound waits on when it starts (the front end,
   the GP and EP DSPs' reset and idle bits, the mailboxes), against xemu's APU registers; make
   every wait it can make finish.
4. **NV2A coverage against xemu's method list:** every PGRAPH method xemu handles that this doesn't
   (Halo's log already shows class 0x39 method 0x180), PFIFO/PTIMER/PCRTC registers D3D reads.
5. **Kernel exports audit:** every one of the 366 against Cxbx-Reloaded's semantics; the ones that
   are stubs or simplified get done properly, each with a kernel-test check.
6. **XNet with no cable:** what the network stack does after the adapter (DHCP timeouts, timers);
   nothing must block a game's start-up.
7. **Everything the logs can tell from a run with no frame:** a watchdog report that names the
   loop a thread spins in (the code around its eip, the device registers it reads).
8. **HANDOFF.md** brought up to date with all of tonight.
9. **Morning summary** below, the final packages sent.

## Done

(each item: what changed, how it was checked, the commit)

- **1 and 2, USB.** Both MCPX OHCI controllers now exist: USB 1 at 0xfed08000 (IRQ 9) with nothing
  plugged in, as PCI reports it. Registers per OHCI 1.0a: reset values on HCR (HcFmInterval,
  HcLSThreshold), HcHCCA's low byte read-only, HcFmRemaining counting down, HcRevision/HcFmNumber
  read-only, interrupt routing never to SMM, frame-number overflow (FNO). The controller's
  internal hub (USB 2.0 ch. 11, 3 ports) sits on root port 3 with the gamepad on its port 1, as
  the console's controller is built and as xemu and Cxbx-Reloaded attach theirs (Microsoft's USB
  stack was written for it). Checked: kernel test (USB 1 answers, HCR, HCCA alignment); nxdk's
  stack enumerates the hub (address 2) then the gamepad behind it (address 3) in the usb and
  sdl_gamecontroller samples; with --pad a the sdl_gamecontroller sample shows "A:1" for
  controller 1 (045e:0202); regression 12/12 both ways.
