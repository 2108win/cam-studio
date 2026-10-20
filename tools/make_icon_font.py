"""Builds the PC app's icon fonts from Microsoft's Fluent UI System Icons (MIT).

The app ships only the few icons it uses, under our own code points, in two fonts with the same
code points: Regular and Filled (Filled for a state that is on: flash, recording). Windows 10 has
no Segoe Fluent Icons, so the fonts travel with the app (app/Assets/Fonts).

Usage (needs fonttools: python -m pip install fonttools):
  1. Download into an empty folder, from github.com/microsoft/fluentui-system-icons (main):
     fonts/FluentSystemIcons-Regular.ttf + .json, fonts/FluentSystemIcons-Filled.ttf + .json, LICENSE
  2. python tools/make_icon_font.py <that folder>

Writes app/Assets/Fonts/CamStudioIcons-{Regular,Filled}.ttf, the licence next to them,
app/Icons.xaml (one string resource per icon: Glyph="{StaticResource IconAdd}") and app/Icons.cs
(the same names for code: Icons.Add). Add an icon by appending a row below (never renumber).
"""
import json
import os
import shutil
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

# Our name, our code point, the Fluent icon (size variant picked for the size it is drawn at).
ICONS = [
    ("Add",              0xE001, "add_16"),
    ("Rescan",           0xE002, "arrow_sync_16"),
    ("Restart",          0xE003, "power_20"),
    ("Settings",         0xE004, "settings_16"),
    ("Video",            0xE005, "video_16"),
    ("PanelRight",       0xE006, "panel_right_16"),
    ("VideoLarge",       0xE007, "video_48"),
    ("Play",             0xE008, "play_16"),
    ("Eye",              0xE009, "eye_16"),
    ("CameraSwitch",     0xE00A, "camera_switch_20"),
    ("Flash",            0xE00B, "flash_16"),
    ("Exposure",         0xE00C, "brightness_high_16"),
    ("Crop",             0xE00D, "crop_16"),
    ("Info",             0xE00E, "info_16"),
    ("ZoomOut",          0xE00F, "zoom_out_16"),
    ("ZoomIn",           0xE010, "zoom_in_16"),
    ("FlipHorizontal",   0xE011, "flip_horizontal_16"),
    ("FlipVertical",     0xE012, "flip_vertical_16"),
    ("Rotate",           0xE013, "arrow_rotate_clockwise_16"),
    ("Color",            0xE014, "color_16"),
    ("Options",          0xE015, "options_16"),
    ("Window",           0xE016, "window_16"),
    ("Phone",            0xE017, "phone_16"),
    ("More",             0xE018, "more_horizontal_16"),
    ("Record",           0xE019, "record_16"),
    ("Stop",             0xE01A, "stop_16"),
    ("Camera",           0xE01B, "camera_16"),
    ("Keyboard",         0xE01C, "keyboard_16"),
    ("QrCode",           0xE01D, "qr_code_20"),
    ("Delete",           0xE01E, "delete_16"),
    ("Blur",             0xE01F, "blur_16"),
    ("Image",            0xE020, "image_16"),
    ("Pause",            0xE021, "pause_16"),
    ("FolderOpen",       0xE022, "folder_open_16"),
    ("BackgroundEffect", 0xE023, "video_background_effect_16"),
    ("EyeOff",           0xE024, "eye_off_16"),
    ("Dismiss",          0xE025, "dismiss_16"),
    ("Open",             0xE026, "open_16"),
    ("VideoOffLarge",    0xE027, "video_off_48"),
    ("PhoneLarge",       0xE028, "phone_48"),
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "app", "Assets", "Fonts")


def build(src, style):
    names = json.load(open(os.path.join(src, f"FluentSystemIcons-{style}.json"), encoding="utf-8"))
    font = TTFont(os.path.join(src, f"FluentSystemIcons-{style}.ttf"))
    cmap = font.getBestCmap()
    wanted = {}
    for name, code, fluent in ICONS:
        key = f"ic_fluent_{fluent}_{style.lower()}"
        if key not in names:
            sys.exit(f"{key} is not in FluentSystemIcons-{style}.json")
        wanted[code] = cmap[names[key]]

    options = subset.Options()
    options.glyph_names = True
    options.notdef_outline = True
    options.name_IDs = []        # the name table is rewritten below
    options.layout_features = []
    sub = subset.Subsetter(options)
    sub.populate(glyphs=sorted(set(wanted.values())))
    sub.subset(font)

    # Our code points only.
    for table in font["cmap"].tables:
        if table.isUnicode():
            table.cmap = dict(wanted)
    font["cmap"].tables = [t for t in font["cmap"].tables if t.isUnicode() and t.format in (4, 12)]

    family = "Cam Studio Icons" if style == "Regular" else "Cam Studio Icons Filled"
    ps = family.replace(" ", "")
    name = font["name"]
    name.names = []
    for nid, value in [(0, "Icons: Copyright (c) 2020 Microsoft Corporation, MIT licence"),
                       (1, family), (2, "Regular"), (3, f"{ps}-1"), (4, family), (5, "Version 1.0"), (6, ps)]:
        name.setName(value, nid, 3, 1, 0x409)
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, f"CamStudioIcons-{style}.ttf")
    font.save(path)
    return family, path


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    for style in ("Regular", "Filled"):
        family, path = build(src, style)
        print(f"{path}: {family}, {os.path.getsize(path)} bytes")
    shutil.copyfile(os.path.join(src, "LICENSE"), os.path.join(OUT, "FluentSystemIcons-LICENSE.txt"))

    xaml = ['<!-- Generated by tools/make_icon_font.py: the app\'s icons (Fluent UI System Icons, MIT).',
            '     Glyph="{StaticResource IconAdd}" with FontFamily="{StaticResource IconFont}" (or',
            '     IconFontFilled for the filled look). Do not edit: change the table in the script. -->',
            '<ResourceDictionary',
            '    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"',
            '    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">',
            '    <FontFamily x:Key="IconFont">ms-appx:///Assets/Fonts/CamStudioIcons-Regular.ttf#Cam Studio Icons</FontFamily>',
            '    <FontFamily x:Key="IconFontFilled">ms-appx:///Assets/Fonts/CamStudioIcons-Filled.ttf#Cam Studio Icons Filled</FontFamily>']
    xaml += [f'    <x:String x:Key="Icon{name}">&#x{code:X};</x:String>' for name, code, _ in ICONS]
    xaml += ['</ResourceDictionary>', '']
    open(os.path.join(ROOT, "app", "Icons.xaml"), "w", encoding="utf-8").write("\n".join(xaml))

    cs = ['// Generated by tools/make_icon_font.py: the app\'s icons (Fluent UI System Icons, MIT), the',
          '// same names as the IconXxx resources in Icons.xaml. Do not edit: change the table in the script.',
          'namespace CamStudio;',
          '',
          'internal static class Icons',
          '{']
    cs += [f'    public const string {name} = "\\u{code:04X}";' for name, code, _ in ICONS]
    cs += ['}', '']
    open(os.path.join(ROOT, "app", "Icons.cs"), "w", encoding="utf-8").write("\n".join(cs))


if __name__ == "__main__":
    main()
