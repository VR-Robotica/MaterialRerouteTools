# Material Reroute Tools

Editor tools for Unreal Engine's material editor that keep big material graphs readable. Every value gets a **named
reroute** whose name prefix and color say its type at a glance, and whole groups of parameters or inputs can be
organized in one click. Editor only: nothing ships with your game.

Supported engine: **5.8**. Platforms: Windows, macOS, Linux (editor).

## Type prefixes and colors

Both are set per project in **Project Settings > Plugins > Material Reroute Tools** (saved to `DefaultEditor.ini`, so
a team shares them). Changes apply to reroutes made afterwards. The defaults:

| Type | Prefix | Color |
|---|---|---|
| Boolean / static bool | `b_` | red |
| Scalar | `s_` | green |
| Vector 2 | `vec2_` | yellow green |
| Vector 3 | `vec3_` | yellow |
| Vector 4 | `vec4_` | orange |
| Texture object | `tex_` | blue |
| Material attributes | `ma_` | purple |
| Shading model | `sm_` | teal |
| Normal map RGB | type prefix | light blue (0.5, 0.5, 1) |
| Unknown type | none | grey |

The type comes from the pin itself where the node knows it (parameters, samplers, function outputs, masked pins),
and is otherwise worked out from the graph: component masks and appends are counted, reroutes and function calls are
followed, and generic math takes its widest input.

## Create Prefixed Named Reroute (drag off any pin)

Drag a wire off a pin and pick **Create Prefixed Named Reroute** (at the top of the menu). Works in materials and
material functions.

- **Off an output:** adds a declaration fed by that pin, named `<prefix><name>`. Every input the pin already fed is
  switched to a usage of the new reroute placed beside it, so long wires disappear. One undo step.
- **Off an input:** adds a declaration where you dropped the wire (taking over whatever fed the input) and a usage
  beside the input.

Names come from what the pin carries:

| Source | Example |
|---|---|
| Scalar / vector / switch / component mask / texture / collection parameter | `s_Roughness`, `vec4_Tint` |
| One channel of a parameter | its custom channel name, else `s_Tint R` |
| Engine input nodes | `vec2_UV0`, `vec3_World Position`, `vec3_Camera Vector`, `s_Time`, `vec4_Vertex Color` |
| Particle nodes, Dynamic Parameter | `s_Particle Relative Time`, `vec4_Dynamic Parameter`, `s_Erode` |
| Material function inputs and outputs | the input / output name |
| Vertex Interpolator | the name of what feeds it |
| Anything else | `<prefix>reroute` |

Pin names with a type tag such as `Light Mask (S)` or `Base Color (V3)` set the type and the tag is left out of the
name, so tagging your own material function pins this way makes them reroute cleanly.

## Right-click node menus (materials)

Under the node right-click menu's **Material Reroute Tools** section:

- **Organize Parameters into Named Reroutes** (selection with parameters): gathers the selected parameters into a
  column at the top left of the graph, gives each value read from them a typed named reroute and replaces the long
  wires with usages.
- **Organize Inputs into Named Reroutes** (Result node, Make / Set Material Attributes): gives each wired input a
  named reroute named after the input, with a declaration beside the source and a usage beside the pin.
- **Create Parameter Data Function** (selection with parameters): moves the selected parameters (scalar, vector,
  texture sample, texture object with its samplers, and appends of them) into a new material function with one
  typed output each, and reads them back here through the function and named reroutes.
- **Add to Parameter Data Function**: moves selected parameters into a parameter data function the material already
  calls, as new outputs.

## Installation

Copy the `MaterialRerouteTools` folder into your project's `Plugins` folder (or install from Fab), then enable
**Material Reroute Tools** in Edit > Plugins and restart the editor.

## Support

Report issues at https://github.com/VR-Robotica/MaterialRerouteTools/issues.

## Making the Fab upload zips (source repo only)

`Scripts/fab_zip.py` builds the Fab upload zip for one engine version from the last commit (commit first: uncommitted
changes are left out). Run it from the plugin folder with Python 3:

```
python Scripts/fab_zip.py 5.8
python Scripts/fab_zip.py 5.6
python Scripts/fab_zip.py 5.6 D:/Builds/MaterialRerouteTools_5.6.zip
```

Each zip is written to the desktop as `MaterialRerouteTools_<version>.zip` unless a path is given, and holds one
`MaterialRerouteTools/` folder with the uplugin, README, Config, Resources and Source (no Binaries, Intermediate or
`Scripts/`), the uplugin's `EngineVersion` set to `<version>.0`. The script stops with an error if a path uses
backslashes, build output got in, or a source file is missing its copyright line. Submit one zip per engine version.

Copyright VR Robotica. All Rights Reserved.
