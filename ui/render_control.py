"""Render the mapping rows and shared action chooser for the MPC skin."""

import json
from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont


BACKGROUND = "ff14161a"
TEXT = "fff3f5f8"
ROWS_PER_PAGE = 8
BUTTON_HEIGHT = 54
ACTION_WIDTH = 264
BINDING_STEPS = 1 + 2 * 16 * 128


def field(obj, key):
    return next(value for name, value in obj if name == key)


def replace(obj, key, value):
    index = next(i for i, (name, _) in enumerate(obj) if name == key)
    obj[index] = (key, value)


def bind_skin(project, skins, sdk):
    sys.path.insert(0, str(sdk / "tools"))
    import gen_ui as ui

    params = json.loads((project / "build/params.json").read_text())["params"]
    handles = {p["name"]: f'Parameter {p["index"]}' for p in params}
    row_count = sum(p["name"].startswith("Target ") for p in params)
    page_count = row_count // ROWS_PER_PAGE
    actions = next(p["choices"] for p in params if p["name"] == "Target 1")
    assert row_count % ROWS_PER_PAGE == 0 and actions[0] == "Choose control"
    controls = []

    def indexed(name, step, count):
        return f"IndexedEnabling/{step}/{count}/{handles[name]}"

    closed = indexed("Picker Visible", 0, 2)
    opened = indexed("Picker Visible", 1, 2)

    def button(name, handle, rect, image, visible, step=1, steps=1, on=None):
        data = ui.button_data(on or image, image, step, steps)
        controls.append(ui.entry(name, "Button", data, " ".join(map(str, rect)),
                                 "No", "Hide", "Always",
                                 remap=[("Data", handles[handle])], invalidating=visible))

    def label(name, rect, visible, handle=None, text=None, size=26, colour=TEXT):
        data = ui.label_data("Value" if handle else "Text", colour, size,
                             "left verticallyCentred", text=text)
        remap = [("Data", handles[handle or "Row Count"])]
        controls.append(ui.entry(name, "Label", data, " ".join(map(str, rect)),
                                 "No", "Hide", "Always", remap=remap, invalidating=visible))

    def artwork(name, caption, width, height=BUTTON_HEIGHT, accent=False, chevron=False):
        image = Image.new("RGBA", (width, height))
        draw = ImageDraw.Draw(image)
        fill, edge, ink = ("#64d9d6", "#64d9d6", "#10171b") if accent else (
            "#252b32", "#505b68", "#f3f5f8")
        draw.rounded_rectangle((0, 0, width - 1, height - 1), radius=8, fill=fill, outline=edge)
        if caption == "−":
            x, y = width // 2, height // 2
            draw.line((x - 9, y, x + 9, y), fill=ink, width=2)
        else:
            draw.text((width // 2, height // 2), caption, font=ImageFont.load_default(size=24),
                      fill=ink, anchor="mm")
        if chevron:
            x, y = width - 26, height // 2
            draw.polygon([(x - 7, y - 4), (x + 7, y - 4), (x, y + 5)], fill=ink)
        png = name + ".png"
        image.save(skins / png)
        return png

    def rows_in_range(first, last):
        return f"RangedEnabling/{first}/{last}/{row_count + 1}/{handles['Row Count']}"

    add = artwork("add", "+", 64)
    remove = artwork("remove", "−", 64)
    dropdown = artwork("action-field", "", ACTION_WIDTH, chevron=True)
    # The value label otherwise takes touches over the text.
    field_hit = "action-field-hit.png"
    Image.new("RGBA", (ACTION_WIDTH, BUTTON_HEIGHT), (0, 0, 0, 1)).save(skins / field_hit)
    momentary = artwork("momentary", "Momentary", 158)
    toggle = artwork("toggle", "Toggle", 158, accent=True)
    learn = artwork("learn", "Learn", 144)
    listening = artwork("listening", "Listening", 144, accent=True)
    unassign = artwork("unassign", "Unassign", 143)
    cancel = artwork("cancel", "Cancel", 152)

    button("Add mapping", "Add Row", (40, 18, 64, BUTTON_HEIGHT), add,
           [rows_in_range(0, row_count - 1)])
    label("Empty mappings", (121, 18, 700, BUTTON_HEIGHT),
          [indexed("Row Count", 0, row_count + 1)], text="Tap + to add a MIDI mapping")
    for page in range(page_count):
        name = f"Page {page + 1}"
        normal = artwork(f"page-{page}", str(page + 1), 56)
        selected = artwork(f"page-{page}-selected", str(page + 1), 56, accent=True)
        button(name, "Page", (900 + 68 * page, 18, 56, BUTTON_HEIGHT), normal,
               [rows_in_range(max(9, page * ROWS_PER_PAGE + 1), row_count)],
               step=page, steps=page_count, on=selected)

    for row in range(row_count):
        number = row + 1
        binding = f"Binding {number}"
        target = f"Target {number}"
        learn_handle = f"Learn {number}"
        mode = f"Toggle {number}"
        visible = [rows_in_range(number, row_count),
                   indexed("Page", row // ROWS_PER_PAGE, page_count)]
        y = 87 + (row % ROWS_PER_PAGE) * 65
        field_image = ui.obj(("version", 2), ("imageType", "Regular"),
                             ("colour", "0"), ("image", dropdown))
        controls.append(ui.entry(f"Action field {number}", "Image", field_image,
                                 f"40 {y} {ACTION_WIDTH} {BUTTON_HEIGHT}", "No", "Show", "Always",
                                 invalidating=visible))
        label(f"MPC control {number}", (55, y, ACTION_WIDTH - 57, BUTTON_HEIGHT), visible, handle=target)
        button(f"Choose control {number}", "Open Picker", (40, y, ACTION_WIDTH, BUTTON_HEIGHT), field_hit,
               visible, step=number, steps=row_count + 1)
        button(f"Input mode {number}", mode, (326, y, 158, BUTTON_HEIGHT), momentary, visible, on=toggle)
        button(learn_handle, learn_handle, (506, y, 144, BUTTON_HEIGHT), learn, visible, on=listening)
        label(f"Assignment {number}", (678, y, 315, BUTTON_HEIGHT), visible, handle=binding,
              size=37, colour="ffd1d8df")
        button(f"Unassign {number}", binding, (1011, y, 143, BUTTON_HEIGHT), unassign,
               [*visible, f"RangedEnabling/1/{BINDING_STEPS - 1}/{BINDING_STEPS}/{handles[binding]}"], step=0, steps=BINDING_STEPS)
        button(f"Remove mapping {number}", "Remove Row", (1176, y, 64, BUTTON_HEIGHT), remove,
               visible, step=number, steps=row_count + 1)

    mapping_controls, controls = controls, []
    background = Image.new("RGB", (1280, 628), "#" + BACKGROUND[2:])
    draw = ImageDraw.Draw(background)
    draw.text((40, 27), "Choose an MPC control", font=ImageFont.load_default(size=28),
              fill="#" + TEXT[2:])
    background.save(skins / "picker-background.png")
    label("Editing row", (361, 24, 600, 40), [], handle="Open Picker",
          size=21, colour="ffd1d8df")
    button("Cancel picker", "Open Picker", (1088, 18, 152, BUTTON_HEIGHT), cancel, [],
           step=0, steps=row_count + 1)
    for step, action in enumerate(actions[1:], 1):
        column, row = (step - 1) % 4, (step - 1) // 4
        png = artwork(f"choice-{step}", action, 289, 52)
        button(f"Select {action}", "Pick Button", (40 + column * 304, 83 + row * 58, 289, 52),
               png, [], step=step, steps=len(actions))

    path = skins / "TUI.json"
    skin = json.loads(path.read_text(), object_pairs_hook=ui.Obj)
    page_data = field(skin, "pageData")
    tab, = field(page_data, "tabs")
    definitions = field(field(page_data, "componentDefinitions"), "localComponentDefinitions")
    panels = []
    for name, children, condition, image in (
        ("Mapping rows", mapping_controls, closed, ""),
        ("Action chooser", controls, opened, "picker-background.png"),
    ):
        component = ui.component_definition([], children, BACKGROUND, bg_image=image)
        definitions.append(ui.obj(("key", name), ("value", component)))
        panels.append(ui.entry(name, name, ui.obj(("version", 1), ("handleName", "Data")),
                               "0 0 1280 628", "No", "Hide", "Always",
                               remap=[("Data", handles["Picker Visible"])], invalidating=[condition]))
    replace(tab, "componentDefinition", ui.component_definition([], panels, BACKGROUND))

    replacements = {
        "Page": [str(i) for i in range(page_count)],
        "Row Count": [str(i) for i in range(row_count + 1)],
        "Open Picker": [""] + [f"Mapping {i}" for i in range(1, row_count + 1)],
    }
    replace(page_data, "replacementStrings",
            [ui.obj(("key", handles[name]), ("value", labels))
             for name, labels in replacements.items()])
    path.write_text(ui.dump_json(skin) + "\n")

    links_path = skins / "Q-Links.json"
    links = json.loads(links_path.read_text())
    links["Program Mode Q-Links"] = {key: -1 for key in links["Program Mode Q-Links"]}
    links_path.write_text(json.dumps(links, indent=4) + "\n")


if __name__ == "__main__":
    sdk, project = map(Path, sys.argv[1:])
    manifest = json.loads((project / "plugin.json").read_text())
    folder = project / "build" / f'{manifest["manufacturer"]} - VST - {manifest["name"]}'
    bind_skin(project, folder / "Plugin Skins", sdk)
