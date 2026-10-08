# GestureSwitch for CONTROL Resonant

Version 1.6

GestureSwitch puts your weapon forms and powers on your controller (and, experimentally, on keyboard and mouse). You don't pick a form in the Loadout and then attack: *how* you press a button picks the form and attacks with it straight away. You can mix forms mid-combo and keep your place in the combo. You can also change how long each weapon's combo is, and how much damage and falter each weapon does.

## Requirements

- **CONTROL Resonant** (PC).
- **f2g DLL Mod Loader (crloader)** by fame2gin. Required: it loads the mod.
- **Mod Settings Menu (CRModMenu) 1.6.1 or newer** by kkyleeb21. Recommended: it is how you change the mod's settings in game. Without it, the mod uses the default settings below.
- **An Xbox controller**, or any controller Windows sees as one. PlayStation controllers should work too, but haven't been tested. Keyboard and mouse support is experimental (see Known limitations).

## Installation

1. **Find the game folder:** the folder that contains `CONTROLResonant.exe`. In Steam: right-click the game > **Manage** > **Browse local files**.
2. **Install the mod loader** by following its own instructions. Afterwards the game folder has `winmm.dll` next to `CONTROLResonant.exe`, and a `crmods` folder.
3. **Install Mod Settings Menu** (recommended) by following its own instructions. It goes in `crmods\CRModMenu\`.
4. **Install GestureSwitch:** extract the download into the game folder, so that you end up with:
   ```
   CONTROL Resonant\
     CONTROLResonant.exe
     winmm.dll
     crmods\
       GestureSwitch\
         GestureSwitch.dll
         gestureswitch.menu.json
   ```
5. **Start the game.** Open **Options > Mods**: the GestureSwitch section should say **Running**.

**If you used WeaponSwitch:** remove it first (its `hid.dll` in the game folder). The two mods control the same things and would fight over them.

## Controls

These are the default buttons. You can change all of them in the menu.

### Primary weapons
Pick how primaries work with **Primary Style**:

| Style | How it plays |
|---|---|
| **Dynamic** (default) | **Press** Primary Attack (X/□) for Cleave, **double-tap** it for Strike (once per swing; mash to keep going), **hold** it for Glaive. Each picks the form and swings. |
| **Modifier** | **LB/L1 + Primary Attack, Secondary Attack or Dash** picks a primary and swings. Primary Attack on its own swings the equipped form with no delay. |
| **Mixed** | Primary Attack works as in Dynamic. **LB/L1 + Primary Attack, Secondary Attack or Dash** only *sets* the primary, without a swing: use it to choose the weapon for your dash attacks. |

In Modifier and Mixed, LB/L1 on its own still does what it does in the game.

**Modifier buttons:** each kind of chord's modifier can be LB/L1, LT/L2, RT/R2 or RB/R1. On **LB/L1** it still does what it does in the game until a chord fires; then the game sees it let go. On **LT/L2, RT/R2 or RB/R1** (the game's power buttons) the game never sees it, so it can't cast a power.

A **dodge attack** (Dash, then Primary Attack) uses your current form.

### Secondary weapons
**RT/R2 + Primary Attack, Secondary Attack or Dash** equips that secondary and attacks with it (default: Extend, Crush, Drill). Hold the button for the heavy attack.

### Combo finishers
**RB/R1 + Primary Attack, Secondary Attack or Dash**, pressed or held, equips one of six finishers. On a combo's third hit the finisher comes out at once. Press the same chord again for its further hits. **Finisher Hold Time** sets how long a chord button is held to count as a hold.

Pick how finishers work with **Finisher Style**:

| Style | How it plays |
|---|---|
| **Dynamic** (default) | Press or hold each chord button: six finishers. |
| **Sets** | The chord buttons always take a press, with two sets of three finishers: set 1 is the Press finishers, set 2 the Hold finishers. The **Finisher Set Button** (default D-pad Left) swaps sets. |

- **Instant Finisher** (off by default): the chord brings its finisher out straight away, at any point in the combo. Mash it to play the finisher again and again.
- **Finisher at End of Combo** (on by default): turn it off and combos start over after their last hit; finishers then only come from the chords.
- **Charged finishers:** a held chord keeps the attack held, so a finisher that charges (Uppercut Surge) charges until you let go. Let go shortly after it comes out for the uncharged version (`charge_grace_ms` in `GestureSwitch.ini` sets how long you have).
- **The two Split finishers** (Split Pivot and Split Orbit) open alike. Partway through one, the other one's chord carries on with *its* second hit.

### Powers
Pick how powers work with **Power Style**:

| Style | How it plays |
|---|---|
| **Dynamic** (default) | **LT/L2 + Primary Attack, Secondary Attack, Dash, Jump, RT/R2 or RB/R1** uses that power slot (1 to 6). |
| **Sets** | **LT/L2 + Primary Attack, Secondary Attack or Dash**, with two sets of three powers: set 1 is the powers set for those three buttons, set 2 the same buttons' powers on the other page. The **Power Set Button** (default D-pad Right, the game's own power-set button) swaps sets. |

The power stays held while you hold LT/L2. While it's held, Jump and Dash work as usual. If you move the Power Modifier to RT/R2 or RB/R1, LT/L2 takes over that button's power slot.

The set buttons still do what they do in the game too.

### Combos
Any mix of weapons makes one combo: three primary hits, then the finisher. Switching weapons mid-combo keeps your place.

In the **Combo** section of the menu, each weapon has its own combo. For each of its three hits, you set how many times it plays (0 skips it). For example, Crush at 0 / 0 / 1 always does its uppercut. **Primary and Secondary Share Combo** makes primaries and secondaries continue one combo instead of two separate ones.

In menus the mod stays out of the way, so holding buttons in menus works as usual.

### Damage
In the **Damage** section of the menu you set how hard your weapon attacks hit, from 0 to 200% of the game's own values: **Damage** (health taken off) and **Falter** (how fast enemies stagger). **Damage Settings** picks how fine-grained:

- **Everything:** one Damage and Falter for every weapon.
- **By Type:** one pair each for primaries, secondaries and combo finishers.
- **By Weapon:** a pair for each of the 12 weapon forms.

Only melee is changed: powers (shield bash, Launch...) keep the game's own values.

### Keyboard and mouse (experimental)
Set **Input Method** (at the top of the menu) to **Mouse and Keyboard**; on **Gamepad** (the default) the mod leaves the keyboard and mouse alone. The chords then also work on keyboard and mouse. Hold a modifier key and press **Left Click** (Primary Attack), **Right Click** (Secondary Attack) or the **Third Key** (default Q):

| Modifier key (default) | Chord |
|---|---|
| **Alt** | Secondary weapons |
| **Ctrl** | Combo finishers, pressed or held |
| **Caps Lock** | Primary weapons (Primary Style Modifier or Mixed) |

Left Click on its own follows the Primary Style, like Primary Attack on a controller. A modifier key still does what it does in the game (Alt still starts and answers radio calls) until a chord fires. Set the keys under **Keyboard & Mouse (experimental)** in the menu; any key works.

The Primary and Secondary Attack keys are the ones in the game's own keybind settings: change them there and the mod follows.

- **Power Style (Keyboard):** **Standard** (default) keeps the game's own six power keys. **Sets:** hold the **Power Modifier Key** (default E) and press Primary Attack, Secondary Attack or the Third Key, with two sets of three powers as on a controller; the **Power Set Key** (default 2) swaps sets. The mod presses the game's own power keys for you, so leave them bound.
- **Finisher Set Key** (default 1): swaps finisher sets in Finisher Style Sets.
- A set key that is also one of the game's power keys only swaps the set.

## Settings

Every section of the menu has its own **Enable** switch: turn a section off and those buttons work as in the game, while the rest of the mod keeps working. The section's other settings are kept for when you turn it back on.

- **Languages:** the menu is in English and Simplified Chinese (简体中文); it follows the game's language. Weapon names stay in English.
- **In game:** **Options > Mods > GestureSwitch**. This covers every assignment and modifier button, the styles, the double-tap and hold timings, the finisher options, the combos, damage and the keyboard keys. Changes apply straight away.
- **Advanced settings:** `crmods\GestureSwitch\GestureSwitch.ini`, created the first time the game runs. It holds the dodge-attack window, button-press timings, a pass-through switch and logging. Each setting is explained in the file, and changes apply while the game runs.

## Uninstalling

Delete the `crmods\GestureSwitch` folder.

## Known limitations

- **Keyboard and mouse support is experimental.** It is only active with Input Method set to Mouse and Keyboard.
- **Damage settings** find a hit's weapon from the game's damage data, or from your last attack for hits that land late. A power used mid-combo is never changed.
- **Switching between two different finishers** takes the old weapon out of your hand straight away; the new one appears when its finisher starts. Secondaries can do the same. The attacks themselves are not affected.
- **PlayStation controller support is untested.** It is built from how the game reads PlayStation pads, but hasn't been tried on a real one yet. Reports are welcome.
- **After a game update,** the mod may no longer find what it needs in the game. It then switches itself off and leaves your controls as the game has them, and says so in its log.

## Troubleshooting

The mod writes a log to `crmods\GestureSwitch\GestureSwitch.log` every time the game starts. Its `ready:` line shows what is working. For a bug report:

1. Set `verbose = 1` in `GestureSwitch.ini`.
2. Play until the problem happens.
3. Send the log.

## Credits

- **Weapon Switch and Randomizer** by Grimpil: the inspiration for this mod.
- **f2g DLL Mod Loader** by fame2gin: loads the mod.
- **Mod Settings Menu** by kkyleeb21: its in-game settings page.
