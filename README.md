# Material Reroute Tools

Editor tools for Unreal Engine's material editor that keep big material graphs readable. Every value gets a **named
reroute** whose name prefix and color say its type at a glance, and whole groups of parameters or inputs can be
organized in one click. Editor only: nothing ships with your game.

Supported engines: **5.6, 5.7, 5.8**. Platforms: Windows, macOS, Linux (editor).

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
- **Export Node to Texture...** (any node with a value output): renders the node's value across a texture's 0-1 UVs
  and saves it as a texture asset, e.g. to bake procedural noise, gradients or masks. Right-click an output pin to
  export that output; a node with several outputs gets a submenu. Choose the size (128 to 4096) and format:
  - **Color (sRGB, 8-bit)**: for colors. Sampling the texture gives the node's values back.
  - **Linear data (8-bit)**: for masks and other 0-1 data (sRGB off; a single value becomes a grayscale texture).
  - **HDR (16-bit float)**: keeps values below 0 and above 1.

  A 4-channel output keeps its alpha. Picking an existing texture in the save dialog updates it, so a node can be
  re-exported after the graph changes. Parameters use their default values, and unapplied edits are included.

  Also works in **material functions**. There the node is rendered as if the function were called with nothing
  wired, so every function input it reads (directly or further upstream) needs **Use Preview Value as Default** on;
  otherwise the menu entry is greyed out and its tooltip names the inputs to change.
  Nodes that read the scene (world position, camera, scene textures, vertex data) have nothing to read on a flat
  render target, so they export as flat or meaningless values.

## Installation

Copy the `MaterialRerouteTools` folder into your project's `Plugins` folder (or install from Fab), then enable
**Material Reroute Tools** in Edit > Plugins and restart the editor.

## Support

Report issues at https://github.com/VR-Robotica/MaterialRerouteTools/issues.

Copyright VR Robotica. All Rights Reserved.
