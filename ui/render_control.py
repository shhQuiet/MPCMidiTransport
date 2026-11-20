"""Render the MIDI Transport control page from the SDK skin."""

import json
from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont


ACTIONS = (("Stop", "STOP"), ("Play", "PLAY"), ("Play Start", "PLAY-START"))
JUSTIFICATION = "left verticallyCentred"
ROW_TOP = 63
ROW_HEIGHT = 118
BUTTON_HEIGHT = 56
UNASSIGN_WIDTH = 156
BINDING_STEPS = 1 + 2 * 16 * 128


def field(obj, key):
    return next(value for name, value in obj if name == key)


def replace(obj, key, value):
    index = next(i for i, (name, _) in enumerate(obj) if name == key)
    obj[index] = (key, value)


def draw_skin(skins):
    panel = Image.new("RGB", (1280, 628), "#14161a")
    draw = ImageDraw.Draw(panel)
    heading = ImageFont.load_default(size=28)
    for row, (_, title) in enumerate(ACTIONS):
        y = ROW_TOP + row * ROW_HEIGHT
        draw.text((52, y + BUTTON_HEIGHT // 2), title, font=heading, fill="#f3f5f8", anchor="lm")
        draw.line((48, y + 87, 1232, y + 87), fill="#333941")
    panel.save(skins / "transport-panel.png")

    font = ImageFont.load_default(size=24)
    for filename, caption, width, fill, edge, color in (
        ("learn.png", "Learn", 182, "#252b32", "#505b68", "#f3f5f8"),
        ("listening.png", "Listening", 182, "#64d9d6", "#64d9d6", "#10171b"),
        ("unassign.png", "Unassign", UNASSIGN_WIDTH, "#252b32", "#505b68", "#f3f5f8"),
    ):
        button = Image.new("RGBA", (width, BUTTON_HEIGHT))
        draw = ImageDraw.Draw(button)
        draw.rounded_rectangle((0, 0, width - 1, BUTTON_HEIGHT - 1), radius=8, fill=fill, outline=edge)
        draw.text((width // 2, BUTTON_HEIGHT // 2), caption, font=font, fill=color, anchor="mm")
        button.save(skins / filename)
    Image.new("RGBA", (UNASSIGN_WIDTH, BUTTON_HEIGHT)).save(skins / "unassigned.png")


def bind_skin(project, skins, sdk):
    sys.path.insert(0, str(sdk / "tools"))
    import gen_ui as ui

    params = json.loads((project / "build/params.json").read_text())["params"]
    handles = {p["name"]: f'Parameter {p["index"]}' for p in params}
    controls = []
    for row, (action, _) in enumerate(ACTIONS):
        y = ROW_TOP + row * ROW_HEIGHT
        binding = handles[f"{action} CC"]
        controls.extend((
            ui.entry(f"Learn {action}", "Button", ui.button_data("listening.png", "learn.png", 1, 1),
                     f"304 {y} 182 {BUTTON_HEIGHT}", "No", "Hide", "Always",
                     remap=[("Data", handles[f"Learn {action}"])]),
            ui.entry(f"{action} assignment", "Label", ui.label_data("Value", "ffd1d8df", 27, JUSTIFICATION),
                     f"542 {y} 350 {BUTTON_HEIGHT}", "No", "Hide", "Always", remap=[("Data", binding)]),
            # Step zero is the unassigned value. Hide the button image at that step.
            ui.entry(f"Unassign {action}", "Button",
                     ui.button_data("unassigned.png", "unassign.png", 0, BINDING_STEPS),
                     f"1018 {y} {UNASSIGN_WIDTH} {BUTTON_HEIGHT}", "No", "Hide", "Always",
                     remap=[("Data", binding)]),
        ))

    path = skins / "TUI.json"
    skin = json.loads(path.read_text(), object_pairs_hook=ui.Obj)
    tab, = field(field(skin, "pageData"), "tabs")
    replace(tab, "componentDefinition",
            ui.component_definition([], controls, "ff14161a", bg_image="transport-panel.png"))
    path.write_text(ui.dump_json(skin) + "\n")

    links_path = skins / "Q-Links.json"
    links = json.loads(links_path.read_text())
    links["Program Mode Q-Links"] = {key: -1 for key in links["Program Mode Q-Links"]}
    links_path.write_text(json.dumps(links, indent=4) + "\n")


if __name__ == "__main__":
    sdk, project = map(Path, sys.argv[1:])
    manifest = json.loads((project / "plugin.json").read_text())
    folder = project / "build" / f'{manifest["manufacturer"]} - VST - {manifest["name"]}'
    skins = folder / "Plugin Skins"
    draw_skin(skins)
    bind_skin(project, skins, sdk)
