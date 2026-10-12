Raw MSP DisplayPort byte streams used to test a flight controller's OSD against DJI O3 Air Unit + Goggles 2.
MSP v1 frames ($M> ... checksum), command 182 only, exactly as a Betaflight flight controller sends them. Play them at
115200 baud on the air unit UART with a heartbeat running, goggles menu "Flight Controller" = Betaflight, HD canvas
(53x20). fastlab.py plays them one MSP frame at a time with 2 ms gaps (groups V, I and R).

prev.bin               CLEAR, "PREVIOUS FRAME" at row 9 col 19, DRAW. Send it first: if the goggles discard the test
                       frame, the previous frame stays and "PREVIOUS FRAME" is still on screen.

The layouts: 74 element strings taken from the real Betaflight OSD element code run in its unit test (a heavy
layout of 45 elements), plus filler rows (see layout{A,B,C}.txt: "row col hex"). All of the frames use the same
106 symbol glyphs.

Frames as the old driver (master) sends them, two screens in a row:
frameA-master.bin      74 writes, 670 cells with spaces (494 without). Goggles: discarded.
frameB-master.bin      frame A plus 4 full rows of 53 characters, 730 cells. Goggles: discarded.
frameC-master.bin      40-character string at row 2 col 0, "WRAPS?" at row 4 col 48. Goggles: shown; row 2 is cut at 30
                       characters, the "?" wraps to row 5 col 0.

Screens as the driver of the pull request sends them (budget 620, load = cells + 0.75 per two-column step + 1.3 per
longer step, in screen order). The first screen after a start merges inner gaps and so may be over budget, the next
ones split every gap, as the driver learns from the previous screen. All of them were shown by the goggles:
v3-frameA0-pr.bin      first screen: load 609 (543 cells), 3 writes dropped.
v3-frameA1-pr.bin      next screens: load 609 (494 cells), every gap split, nothing dropped. Full layout plus rows 16-17
                       of X and Y (40 characters each).
v3-frameB0-pr.bin      first screen: load 609, 5 writes dropped.
v3-frameB1-pr.bin      next screens: load 581 (467 cells), 3 writes dropped: rows 16-17 of digits present, 18-19 missing.
v3-frameC1-pr.bin      40-character string sent as 30 + 10, "WRAPS" clipped at col 52: no wrap.
v3-frame{A,B,C}-pr-3frames.bin   three screens in a row, as sent by the driver.

Single frames (one CLEAR ... DRAW), real element strings with the same 106 symbols, for the load model:
single-A0.bin          590 cells: 414 non-space + 176 spaces. Goggles: discarded.
single-A1.bin          494 cells, no spaces. Shown.
single-B1.bin          573 cells, no spaces, rows 16-18 full width. Discarded. The first 118 of its 120 writes (520 cells)
                       are shown, the first 119 (550 cells) are discarded.
In INAV mode single-A1 and single-B1 are also discarded, v3-frameB1-pr.bin is shown and v3-frameA0/A1 are discarded.
