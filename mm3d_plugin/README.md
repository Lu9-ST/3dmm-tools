# 3DMM actor exporter for Maverick Model 3D

Exports a model to a 3D Movie Maker actor or prop in a `.vxp` v3DMM expansion package
(File > Export > `*.vxp`).

## Install

Copy `3dmm.so` to `~/.mm3d/plugins/1.3/` (create the folder if needed) and restart MM3D.
Plug-in was built against the source of MM3D 1.3.15 + Qt 5.15, have not tested on other versions, but you can build against any MM3D source tree:

    ./build.sh /path/to/mm3d-source /path/to/mm3d-build-dir    # the dir that contains config.h

## What becomes what

| MM3D                                | 3DMM                                                                  |
| ----------------------------------- | --------------------------------------------------------------------- |
| groups                              | a body part (its own model, can have its own texture too)             |
| group name with suffix `#N`         | body part set N ("hat" icon, e.g. `hat#1`)                            |
| material named `xyz`                | that part's texture                                                   |
| materials named `xyz~anything`      | extra skins/costumes for `xyz`, in material order                     |
| frame animations                    | actions (one cel per frame); parts that don't distort share the model |
| skeleton (joints + vertex bindings) | one part per joint, parents first; skeletal animations become actions |
| no animation                        | only "At Rest"                                                        |

## Skeleton support

If the model has joints, every triangle goes to the joint most of its vertices are bound to (the vertex's primary
influence, i.e. the joint with the biggest weight. 3DMM parts are rigid, there is no blending), and each joint becomes a
part that follows its parent joint. A group that spans several joints is split into one part per joint, all sharing
the group's texture. Joints without triangles become empty parts. Skeletal animations are sampled once per frame into
cels. Frame animations still work (a model may have both, the skeleton then stays at rest during frame animations).
Triangles of unbound vertices stay on the root. Seams between joints are not blended, keep that in mind
(overlapping limbs, caps at joints).

Setting `3DMM_SKELETON` `0` ignores joints and exports by group as before. (More on that below)

Set `#N` on group names as usual to choose which costume set a piece belongs to. Empty joint parts are in set 0.

## Animations

An animation name ending in `*` (or `*1.2`) makes it a _moving_ action: the actor travels along its path while it
plays, advancing that many 3DMM units per cel (by default: Walk Fast = 1.0, Run = ~2.0, Walk Slowly = ~0.55).
The action is saved as "Name *" which is how 3DMM does it.
`3DMM_STEP:<name>` = distance does the same from metadata,
`3DMM_STEP` sets the default step (1.0). Without a `*` the action is assumed stationary.

An animation named "At Rest" is created automatically, but you can make an animation with that name to override it.

## Textures

Textures keep their size (rows are padded to a multiple of 4 pixels like the stock textures), and are converted to
the shared 3DMM palette. `3DMM_TEXSIZE` sets a maximum width/height, `3DMM_TEXPOW2` = `1` rounds sizes to powers of
two. Pixels with alpha below 50% become the texture's one transparent color (palette index 0).

Faces with no group become a part called "ungrouped". Color (non-texture) materials become flat textures.

## Default actor mode (experimental)

`3DMM_ACTOR_ANIMS` set to `1` will give you this. This will base your model on 3DMM actor Frank and his 27 body parts
and giving it all of his 33 animations (walk, run, dance, karate kick, etc.)
It needs the game's `tmpls.3cn`: set `3DMM_STOCK` to its path (or its folder);
it is also looked for next to the model and in the usual 3DMM/3DMMEx install places.
The exporter only copies Frank's part tree, body part sets and the motion of the parts.

Put your model in a pose close to Frank's (or give it an `At Rest` animation with that pose: the geometry is
placed in the pose of its first frame). The model is scaled to Frank's height (`3DMM_HEIGHT` overrides) with its feet
on the ground.

Which stock part a triangle goes to: with a skeleton, the joint it is bound to (a joint with an unknown name uses its
closest known ancestor), if no skeleton, then the group name. Names are case-insensitive, `L`/`R`/`left`/`right` can be a
separate word or a suffix (`armL`, `Hand.R`, `bip01 L Thigh`), digits are ignored, and a prefix is ignored
(`blahblah:Hips`). A name without a side uses the side the geometry is on. Unknown names put the triangles on the
torso (the log lists them). `3DMM_MAP:<joint or group name>` = `part name/number (0-26)` overrides it.

| stock part                       | names                                                                                           |
| -------------------------------- | ----------------------------------------------------------------------------------------------- |
| 0 head                           | head, skull, face, neck                                                                         |
| 7 hat slot                       | a **group** (not joint) named hat, cap, helmet, accessory, glasses, sunglasses, crown, headwear |
| 8 torso                          | torso, body, chest, trunk, spine, belly, stomach, abdomen                                       |
| 9 torso back / 10, 11 collar     | torso back, body back / collar, shoulders / collar back                                         |
| 1, 3 ear (L, R) and 2, 4 ear tip | ear, ear tip                                                                                    |
| 5, 6 eye (L, R)                  | eye, eyes, eyeball                                                                              |
| 17, 14 upper arm (L, R)          | upper arm, shoulder, arm, bicep                                                                 |
| 15, 12 forearm (L, R)            | forearm, lower arm, elbow                                                                       |
| 16, 13 hand (L, R)               | hand, wrist, palm, fist, finger                                                                 |
| 18 hips                          | hips, hip, pelvis, waist (triangles above the stock hip line go to the torso)                   |
| 21, 24 thigh (L, R)              | thigh, upper leg, leg                                                                           |
| 19, 22 shin (L, R)               | knee, shin, calf, lower leg                                                                     |
| 20, 23 boot (L, R)               | ankle, boot                                                                                     |
| 25, 26 shoe (L, R)               | foot, feet, shoe, sole, toe                                                                     |

(left = the character's left; for a model facing +Z that is +X. A joint whose L/R word is on the wrong side makes the stock motion mirrored, the export log warns about it.) Parts you do not fill stay empty. A joint called spine (or torso, chest ...) marks the waist: the torso turns around it, and geometry bound to the hip joint is split into hips and torso at its height (without one: at the hip joint).

Joints whose name is known also tell the exporter where the pivot of that part is, so the motion turns around _your_ shoulder, hip, ... instead of Frank's.

### Proportions and size

Your actor does not have to match/look like Frank or any other actor. Joints named like stock parts tell the exporter where
_your_ shoulder, elbow, hip, neck, etc. are, and the motion turns around those. The head, torso, hips and shoes are
separate pieces in the stock data (they are only held together by Frank's proportions), so the exporter ties them
into a chain of its own (hips → torso → head, boot → shoe).

The actor is fitted to Frank's height unless you set `3DMM_HEIGHT`. Jumps and other slides of the stock motion are scaled by your height / Frank's height (`3DMM_MOTION_SCALE` overrides that factor).

### Costume sets

They come from Frank: set 0 head/ears/eyes/hands, 1 hat, 2 torso/arms, 3 legs, 4 shoes. `~` skins work as
before. **Hat slot:** triangles in a group called `hat` (or the other names above) become the hat. `hat@2`, `hat@3`
are further hats the hat tool jumps between; `rig_hat~green`-style skins of a hat's material are costumes of that hat,
and there is always a "no hat" choice at the end. (An accessory's model is swapped by the costume, so it can only follow
its parent part rigidly - that is what the stock hats do too.)

## Options

Assign these from `Edit → Model Meta Data`, all are optional.

- `3DMM_NAME` = actor/prop name (default: file name)
- `3DMM_AUTHOR` = expansion author (v3DMM) (default: none)
- `3DMM_TYPE` = `actor`, `prop` (default: actor)
- `3DMM_FORWARD` = which MM3D axis is the front of the model: `+Z`, `-Z`, `+X`, `-X` (default: +Z, what the MD2 importer produces)
- `3DMM_HEIGHT` = auto-fit height in 3DMM units (default 10; a stock actor is about 12)
- `3DMM_SCALE` = explicit multiplier (disables auto-fit) (default: 1)
- `3DMM_ORIGIN` = `keep` to keep your origin instead of putting the feet on the ground (default: unset)
- `3DMM_MOTION_SCALE` = multiplier for default actor mode, factoring for the stock motion's jumps/slides (default: your height / the stock actor's height)
- `3DMM_DITHER` = `1` for Floyd-Steinberg dithering on textures (default: plain nearest colour posterize)
- `3DMM_DITHER:<material name>` = `0`/`1` overrides the dither option for one texture (its `~` skins follow)
- `3DMM_TEXSIZE` / `3DMM_TEXPOW2`: see [Textures](/mm3d_plugin/README.md#Textures)
- `3DMM_PREVIEW` = path to an image file for the picker thumbnail (default: a front render of the model)
- `3DMM_ANIMS` = `none` to skip animations (default: unset)
- `3DMM_PROP_ANIMS` = `1` appends the stock prop animations (Grow, Grow Fast, Shrink, Shrink Fast, Spin, Spin Fast, Squash, Stretch) unless you already have one with that name (default actor mode also has them) (default: unset)
- `3DMM_ACTOR_ANIMS` = `1` will enable default actor mode and map the default actor animations to joints. (See [Default actor mode (experimental)](</mm3d_plugin/README.md#Default%20actor%20mode%20(experimental)>)) May need the following option too. (default: unset)
- `3DMM_STOCK` = path to `tmpls.3cn` (only needed by default actor mode) (default: common paths to `Microsoft Kids/3D Movie Maker`)
- `3DMM_DWR` = distance per cel of stationary actions (default 2.5)
- `3DMM_CNO` = chunk number base (default derived from the name)
- `3DMM_FIT`, `3DMM_FIT_TOL`, `3DMM_SKIN`, `3DMM_SKELETON`, `3DMM_STOCK_ACTOR` (only Frank's layout is supported) and `3DMM_MAP:<name>`: see [Default actor mode (experimental)](</mm3d_plugin/README.md#Default%20actor%20mode%20(experimental)>).

## Windows support

I'm on Linux, so I built `3dmm.so`. To build a Windows version, `mm3d.exe` has to export functions that a DLL can call, and the pre-built release doesn't. The plug-in needs 40 of them, they're in `win/mm3d_exports.def`).

For a Windows build: build Maverick Model 3D with MSYS2 as described in its `INSTALL.WIN32`, link it once with the export list and an import library (the exact command is in the header of `build_win.sh`), then run `build_win.sh` in the "MSYS2 MinGW 64-bit" shell and copy `3dmm.dll` to `plugins\1.3\` next to `mm3d.exe`. The sources compile for Windows and the DLL links against the export list in a cross-compile here, but it has not been run on Windows yet.

`tmpls.3cn` is looked for next to the model, in `3DMM_STOCK`, and in usual 3DMM / 3DMMEx folders.

## Credits

Made mostly with LLM assistance. Documentation rewritten by me.
All help de-slopifying is welcome.
