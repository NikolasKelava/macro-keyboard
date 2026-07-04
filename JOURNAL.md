# Macro Keyboard — Build Journal

Devlog for my **Hack Club Horizons** hardware submission: a custom Bluetooth LE
macro keypad (12 keys + magnetic rotary encoder + OLED) built on an nRF52840
module and running ZMK.

Time is tracked with **Hackatime** (Wakatime → Hackatime) for firmware work since April.
Hardware/CAD/research sessions are recorded as **timelapses** (Lapse) and linked
per entry, since Lookout isn't supported for Horizons yet.

I have started this project a long time ago (more than a year), but since my time is more or less tracked since April
via Hackatime this journal for the project also starts with entries since april.


## Entries

#### July 1: Fixed the LiPo not being charging

I got the LiPo battery of my macro keyboard to charge, so that's pretty neat.

Observation: When supplying the keypad with power, the charging indicator light up on the screen, but the
screen widget for the battery capacity would only show a static battery percentage.

What could cause this problem? Short answer: The battery not being charging, bruh
1. Faulty screen indicator
2. Faulty resistors in the voltage divider
3. Charging IC not charging the LiPo: Could be due to something disabling the IC.

I was able to eliminate both cause #1 and #2 by measuring if the voltage of the battery increased when
supplying the charging IC. And it did not, this means the cap of the LiPo also stayed the same.
So it must be #3.

![](journal-assets/2026-07-01-LiPo-Voltage.jpeg)

Because the ICs are usually fine, I looked into the schematic of the keyboard to see if I messed up something
there:

![](journal-assets/2026-07-01-Schematic-Charging-IC.png)

So I used the exact same IC before and it worked flawlessly. The only thing that I changed was that I made
use of the temperature sense terminal.
I looked into the datasheet of the IC and what could cause the charging IC to be disabled:

![](journal-assets/2026-07-01-Datasheet-Charging-IC.png) 

The TS (temperature-sense) is the BQ24040's veto over charging. When the NTCs resistance (changes
resistance with temperature of the bat pack) is not in the range that the BQ24040 likes, the battery is 
either to cold/absent or to hot. - Bc of safety, i wanted to implement it.
I measured TS-to-GND and got 237 kΩ. The key insight is from the datasheet is:
The IC pushes a fixed 50 µA through whatever's on TS and watches the *voltage*. At 50 µA a healthy 10 k NTC
sits around 0.5 V, dead center of the charge window. My 237 kΩ slams that node way
past the ~1.6 V "thermistor removed / freezing" threshold, so the IC decided
the battery was impossibly cold (or absent) and quietly disabled charging.

![Multimeter reading ~237 kΩ from the TS pin to GND — the smoking gun](journal-assets/2026-07-01-TS-239.jpeg)

R6 is 240 k and it's supposed to sit in *parallel* with the pack's 10 k NTC → 10k ∥ 250k ≈ 9.6 kΩ.
If the NTC were actually in the circuit I could never measure *above* 10 k. Reading 237 k meant
the thermistor's return path was gone.

But why is the pack's NTC was never electrically present?
I asked Claude:
1. Either cold joints
2. BAT– isn't tied to the same GND R6 references → the NTC has no return path
3. The pack's third wire isn't a 10 k NTC.

And guess what? Yes, it was #3 again. I added the third wire (NTC) to the battery myself, because the battery
came without it. And I thought it was no problem as the BMS of the battery clearly labeled a third pad "NTC",
but it seems like the pad was just connected to no thermistor, because after measuring the resistance between
NTC and GND directly, there was none. Haha

Solution:
I don't need JEITA temp protection for the keypad to work (I will just get a LiPo with 3 wires the next time),
so I took the datasheet's sanctioned shortcut: a fixed 10 kΩ from TS to GND to pin the node back into the
normal window. In parallel with R6 that's ~9.6 kΩ → ~0.48 V on TS → charging
enabled.

![The 10 kΩ resistor tacked in from TS to GND](journal-assets/2026-07-01-ts-fix-10k.jpeg)

Plugged in USB and the charge status finally went active — the pack is pulling
current for the first time.

![Not charged](journal-assets/2026-07-01-noncharg.jpeg)
![Charge status showing the pack actually charging - after a few minutes](journal-assets/2026-07-01-charg.jpeg)

**tl;dr:** LiPo wouldn't charge because the BQ24040's TS pin saw 237 kΩ (the
NTC was never in-circuit) and disabled charging. A 10 kΩ from TS to
GND put it back in range and enabled charging. For new keypads, I will just get the batteries with
3 wires.

**Time spent this session: 4 hours**


#### July 2: Wrote the BOM

I added and wrote BOMs (yes, plural) to fill some gaps in the repo and make the macro keyboard reproducible.

I noticed, that the shipping instructions for my project specifically ask for a bill of materials (BOM) to enable others to reproduce it. The current version doesn't have one, former ones had.

Why BOMs? - I landed on a multi-BOM structure (or idk how to call it):
- One BOM for PCBA under /hardware/PCB/KiCad/Macro-Keyboard-v4/production/bom.csv: After manufacturing the PCB, its populated with the components in this BOM. This step is the PCB assembly. Only the parts for this manufacturing steps are in this BOM. (e.g. nRf module, caps, resistors, usb port, etc.)
- PCB BOM under /hardware/BOM_PCB: Some of the parts, that need to be soldered onto the PCB, can't be soldered in the PCBA step, usually because those parts are to big. So I solder those manually. (e.g. key switches, OLED display, JST connector for LiPo - that's all)
The production BOM (for PCBA) captures only the parts for PCBA and the PCB BOM captures all of the parts, that need to be soldered onto the PCB, including the parts from PCBA.
- Master/build cost BOM: This BOM lists all of the on-PCB parts, which couldn't be populated with PCBA, + PCBA + the PCB itself (isn't included in neither PCB BOM or PCBA BOM) + all of the off-PCB parts. (e.g. enclosure + screws, the LiPo battery, bearings, keycaps, etc.)
This explains the structure. I will put this also in the README.md (probably in shorter form).

Only in the master/build cost BOM I added in the approximate costs for the components. I didn't also add prices to the PCB BOM, because PCBA cost might very, some components might need to be exchanged for other ones, tax and shipping costs can also change. Shipping cost, tax, customs duties are also listed as they are quite substantial.
In the end of the master BOM I calculated the approximate price per keyboard unit (without labour), which turned out to be about 61,38€. That could have been worse. But it needs to be said: If you somehow decide to build one on your own, you will end up paying more then just the price for one unit, because there are minimum ordere quantities for many components (above of what is needed for one keyboard unit). For example, the PCBs need to be ordered in quantity of 5 or above with JLCPCB.

To the main BOM I also added Notes to clarify in which group the components belong (on-PCB, on-PCB PCBA, off-PCB) beside the vendor.

![Picture of cost BOM](journal-assets/2026-07-02-BOM.png)

**tl;dr:** idk bro, that shi just tedious work

**Time spent this session: 3 hours**
