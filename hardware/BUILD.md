# Build & Assembly Guide

Full instructions to build one Macro Keyboard from scratch.

This guide is for the enclosure version #1 with PCB version #4 of the PCB (v4.1).
This PCB version currently has 2 major issues, which will be resolved in a future revision,
which is not released currently - this guide will help you fix these issues.
If you can wait on a revision (whose release date is not guaranteed), I would, because the
fixes are not beautiful.

## 1. Parts & tools

**Parts:** source full list in [BOM.csv](./BOM.csv); (on-PCB component list under [BOM_PCB.csv](./BOM_PCB.csv))

**Tools**
- for enclosure fabrication: 3D printer
- for soldering: soldering iron + solder + solder mat (+ flux + soldering wick)
- for assembly: TX6 screw driver (for screws) (+ double sided tape) 
- for first bootloader install: SWD programmer (DAPLink/pyOCD) or JTAG with [Cortex Debug (10-pin) connector](https://developer.arm.com/documentation/101416/0100/Hardware-Description/Target-Interfaces/Cortex-Debug--10-pin-)
- for alignment of the SwitchPlate: 4 M2 nuts

## 2. Order the PCB

Read up a little on PCB manufacturing so you know what PCBs are, which manufacturer 
to choose, and what settings to specify when you order the PCBs.
I used JLCPCB because they also offer a PCBA service with components that I can either 
send to them or have sourced directly from their LCSC warehouse, which makes it easy 
to select parts and have them assembled right away.

You can also assemble the components yourself, but I didn’t dare to do it because the
SMD components often have very small pads and it’s easy to damage them. Besides the risk,
there’s little point in doing it yourself because it isn’t significantly cheaper.

1. Fabrication files: Gerbers are under [production/Macro-Keyboard-v4.zip](./PCB/KiCad/Macro-Keyboard-v4/production/);
                      `bom.csv` and `positions.csv` for PCBA are under [production/](./PCB/KiCad/Macro-Keyboard-v4/production/)
                      
2. Upload Gerbers to JLCPCB; choose board options - the Gerbers usually carry the relevant information on the PCB,
like size, layer count, via positions, etc. Check that the layer count is set to 4 and the manufacturer can support
the required tolerances and other parameters with the selected service. - The manufacturer usually provides these
under their capabilities (have a look at there website).
My PCBs were manufactured without any problems by JLCPCB.

3. For SMT assembly, submit the parts in [bom.csv](./PCB/KiCad/Macro-Keyboard-v4/production/bom.csv) 
with the positions file. 
I checked that in this BOM only the parts to be populated are included, but checking again doesn't hurt.
So if you notice (for example), that mx key switches are included, take them out.
The parts, that should not be populated and are on the PCB, are marked as DNP (do not populate) in the
[BOM_PCB.csv](./BOM_PCB.csv): key switches, OLED display, JST connector for LiPo
Pic the top side of the PCB to be populated, check the capabilities of the SMT assembly service of your
manufacturer and then you should be good to go.

## 3. Print the enclosure + knob + keycaps

Case files: [CASE/](./CASE/)
There you can find the mesh exports (as .obj files) from CAD, the Autodesk Fusion source (my CAD program of choice) 
and my Bambu Studio project (if you wonder how to rotate the parts).
The enclosure is designed for FDM 3D printing.

Material:
I would recommend PETG for the Switch-Plate, because it needs to be more flexible then the rest of the enclosure
Everything else of the enclosure can be printed in every other rigid filament of your choice. I used PLA.

Print Bed:
I would recommend a smooth print bed for the surfaces of the prints.

In terms of print settings:
Those are not parts, which need to withstand a lot of force, nor do they need to be the most dimensionally
accurate: I designed the parts with tolerances for my FDM 3D Printer (a Bambulab P1S). If yours needs different
tolerances you can adjust them in the Fusion Source either in the related sketch or by moving the face manually.

For the whole enclosure and knob there are no supports need, but I would recommend to add support to the TOP part
at the place in the attached picture.

Place supports, where the print is colored dark blue:
![](assets/build-assets/2026-07-25-Print-Support-Placement.png)
This is how it looks sliced:
![](assets/build-assets/2026-07-25-Print-Support-Sliced.png)
This is how the part looks after the supports are removed:
![](assets/build-assets/2026-07-25-Print-Support-IRL.jpeg)

Now Print the enclosure + knob + keycaps!

## 4. Assembly + Hand Soldering

All of the components are under the [BOM.csv](./BOM.csv). Make sure you have them, before you continue.

### Preparation: 
Before you can assemble the macro keyboard you need to
- install 4 threaded inserts into the TOP enclosure part.

![](assets/build-assets/2026-07-25-Inserts-Location.jpeg)
- put the magnet into the slot of the knob + add the bearing.

![](assets/build-assets/2026-07-25-Knob.jpeg)
![](assets/build-assets/2026-07-25-Knob-side.jpeg)
- install double sided tape on the battery cover.

![](assets/build-assets/2026-07-25-bat-cover.jpeg)
- install double sided tape in the BOTTOM of the enclosure and put the battery in.

![](assets/build-assets/2026-07-25-bat.jpeg)

Now you have all of these parts in front of you:
![](assets/build-assets/2026-07-25-Overview.jpeg)
(I already put the key switches and OLED display in place.)

### Hand soldering the remaining parts:
The parts marked **DNP** in [BOM_PCB.csv](./BOM_PCB.csv) are soldered manually by hand after the board arrives:
key switches, JST battery connector, SWD header, OLED module

Secure the SwitchPlate with 4 M2 nuts and make sure the Plate is aligned.
Then just pop in the Switches and solder them on from the backside of the PCB:

![](assets/build-assets/2026-07-25-Switch-Soldering.jpeg)

Put on the TOP of the enclosure and make sure the OLED is aligned: (When putting on the TOP, make sure to watch the power switch.)

![](assets/build-assets/2026-07-25-OLED-Alignment.jpeg)

Solder the OLED.

Put in the JST connector on the bottom of the PCB and solder it.
But!!!! With the current version of the PCB, the JST XH2 connector has wrong polarity, because I forgot to flip
the blueprint in the PCB design! So what you will do is: Rotate the pins on the horizontal connector by 180°,
so when putting in the connector, it faces the right way into the PCB!!! (If you don't the polarity doesn't match
and you will notice it even before you get a short, because the connector would face outside of the PCB.)

![](assets/build-assets/2026-07-25-JST.jpeg)

This is what it should look like.

I will release another version of the PCB, with 2 Fixes:
1. Moderate issue: "July 1: Fixed the LiPo not being charging" in JOURNAL.md
2. Plus the power delivery issue (brownout) that takes down the whole periphere of the PCB: OLED and AS5600 encoder

If I haven't removed this block, you will need to read the corresponding JOURNAL.md entry and it's fix.

### Assembly:

Put on the battery cover like so:

![](assets/build-assets/2026-07-25-bat-cover-on.jpeg)

Then connect the battery to the JST connector:

![](assets/build-assets/2026-07-25-JST.jpeg)

Then align the two halves of the enclosure as followed:

![](assets/build-assets/2026-07-25-2halves.jpeg)

Tighten down all 4 screws alternating:

![](assets/build-assets/2026-07-25-screwing.jpeg)

Then you will be left with this:

![](assets/build-assets/2026-07-25-base-wo-caps.jpeg)

Put on the keycaps:

![](assets/build-assets/2026-07-25-keycaps.jpeg)

Press the knob into the enclosure and make sure the knob spins freely.

![](assets/build-assets/2026-07-25-knob-1.jpeg)
![](assets/build-assets/2026-07-25-knob-2.jpeg)

### Assembled:

![](assets/build-assets/2026-07-25-assembled.jpeg)

## 5. Flash the firmware

### Notice:
- It can be that by the time you read this, that I have added the build artifacts for my firmware to GitHub.
  If so and you don't want to make your own modifications, you can just skip step #2 and flash the
  build artifacts I provide. 
  (Keymap can still be configured in ZMK Studio with studio version - default build will come without key assignments)

- I recommend to install the studio version as this firmware version makes configuring over ZMK Studio possible.

- Use whatever flashing protocol and program is supported by your debugger. With my DAPLink Debugger I use pyocd via SWD, which I conveniently installed into the venv of my local toolchain.
  This guide will be written for pyocd.

- I would recommend to install the UF2 bootloader as it will make your life easier later. If you ever need to update the firmware on the board later, you won't need to flash over SWD again.
  If you don't want to install a bootloader and want to flash the firmware directly with your debugger, read the section in [firmware/CLAUDE.md → Bootloader](../firmware/CLAUDE.md) "If the bootloader has to be removed".

- Follow the official [ZMK firmware document](https://zmk.dev/docs/development/local-toolchain/setup) on setting up the local Zephyr toolchain to build the ZMK firmware with macro_keyboard config. Install the current version of ZMK, which is added as a submodule to this repo. Install under the path 'firmware/zmk_toolchain/'.

### 1. **First-time bootloader install** (SWD/DAPLink with pyocd):
(also in [firmware/CLAUDE.md → Bootloader](../firmware/CLAUDE.md).)

A UF2 bootloader (Adafruit_nRF52_Bootloader, MDBT50Q-1MV2 build) is the default flashing target. The binary lives at `firmware/bootloader/macro_keyboard_bootloader.hex`.

The build is wired for it by default — see `macro_keyboard_defconfig`:

```
CONFIG_USE_DT_CODE_PARTITION=y       (links app at code_partition = 0x26000)
CONFIG_BUILD_OUTPUT_UF2=y             (emits zmk.uf2)
CONFIG_BUILD_OUTPUT_UF2_FAMILY_ID="0xADA52840"   (Adafruit nRF52840 family ID)
```
**Bootloader install / re-install** (one-time, via SWD/DAPLink and pyocd):

Connect the debugger probe (you will need to take of the TOP of the enclosure) by pressing in the 10 pins of your probe into the 10 pins of the PCB (beside the MCU). Make sure to connect from the upper-side of the PCB and the direction of the connector will need to face inside the PCB.

Then run: (before you can "pyocd list" to list the debug probes with targets)

```bash
pyocd flash --target=nrf52840 \
  /Users/nikolaskelava/Documents/macro_keyboard/firmware/bootloader/macro_keyboard_bootloader.hex
```

After this, double-tapping reset mounts the USB MSC drive.

### 2.1 **Preparing the /config before building locally** with the toolchain:

If you choose the studio version, you don't need to configure the keymap in /config.

If you don't, you need to configure your keymap now, before you build.

### 2.2 **Build `zmk.uf2`**:
(also in [firmware/CLAUDE.md → Local Builds](../firmware/CLAUDE.md)
   or the [README build section](../README.md#firmware--build--flash))

Build (from `firmware/zmk_toolchain/app`, ZMK toolchain):

```bash
source /firmware/zmk_toolchain/.venv/bin/activate

west build -p -d build/macro_keyboard/m3 -b macro_keyboard -- \
  -DZMK_CONFIG="$PWD/../../zmk-config/config" \
  -DZMK_EXTRA_MODULES="$PWD/../../zmk-config/module"
```

Add `-S studio-rpc-usb-uart -DCONFIG_ZMK_STUDIO=y` for the ZMK Studio variant.
The build emits both `zmk.uf2` (drag-drop) and `zmk.hex` (SWD via `west flash -r pyocd`).

### 3.1 **Flash with UF2**:

Take off the knob and double-tap reset (mounts as `NRF52BOOT`) and drag `zmk.uf2` onto it.

### 3.2 The board will be dismounted and **boot into the firmware**.

### (4.0 Install Mac Mouse Fix or Linear Mouse on macOS:)
    With the current config the macOS scroll acceleration, which is applied to all scroll wheels, is awful.
    If you want to use scrolling, you can fix it by:
    1. Installing a 3rd party application like "Linear Mouse" to take out the scroll acceleration. But Linear Mouse will do so by applying a fixed step size per tick. This means that you wont be able to scroll as precisely.
    2. If you don't want any 3rd party app intersecting: In the /config you can configure the keymap node: scroll-triggers-per-rotation = <120>; to a lower trigger count. But this means, that it will also get less smooth.
    3. I landed on the application "Mac Mouse Fix". It works as expected by removing the scroll acceleration and enabling smooth scrolling multiplier.

## 6. Verify

- OLED shows battery + connection type on boot.
- Pair over BLE (or plug USB); the host sees an HID keyboard.
- Each of the 12 keys registers on the host.
- The encoder acts on the active profile's mode (volume / scroll / tabs).
- The profile button cycles profiles; the mode key cycles encoder modes. - with default setup.
- Battery %/charging indicator responds to USB plug/unplug.

- If you have installed the studio version:
  Plugin the macro keyboard via USB, go to zmk.studio, configure the keymap, upload it to the board and verify that the
  key presses execute the macros you configured.

## Safety

See [README → Safety notes](../README.md#safety-notes) — LiPo handling, 3.3 V
logic, USB-C power.
