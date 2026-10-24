bl_info = {
    "name": "3DMM Scene Maker",
    "author": "Lu9",
    "version": (0, 2, 1),
    "blender": (3, 6, 0),
    "location": "Properties > Scene > 3DMM Scene Maker",
    "description": "Render Blender cameras as Microsoft 3D Movie Maker scenes (image + depth) and export as v3DMM expansions",
    "category": "Import-Export",
}

import os, tempfile
import numpy as np
import bpy
from bpy.props import (StringProperty, FloatProperty, BoolProperty, IntProperty, EnumProperty,
                       PointerProperty, CollectionProperty)
from bpy_extras.io_utils import ImportHelper
from mathutils import Vector
from . import chunky, bkgdwrite, coremath as C, exporter as E, scenebuild as S, midi as MIDI
import importlib
for _m in (chunky, bkgdwrite, C, S, MIDI, E):          # pick up edited modules when the addon is re-enabled/reinstalled
    importlib.reload(_m)

W, H = C.W, C.H
NO_MUSIC = "(none)"


# ---------------------------------------------------------------- properties
def _snds_path(st):
    b = bpy.path.abspath(st.bkgds_path)
    return os.path.join(os.path.dirname(b), 'snds.3cn') if b else ''

def _song_search(self, context, edit_text):
    try:
        names = sorted(S.stock_songs(_snds_path(self)).keys(), key=str.lower)
    except Exception:
        import traceback; traceback.print_exc()
        names = []
    q = edit_text.lower()
    return [n for n in [NO_MUSIC] + names if q in n.lower()]


class DMMCamItem(bpy.types.PropertyGroup):
    name: StringProperty()                       # camera object name
    use: BoolProperty(name="Use", default=True)


class DMMSettings(bpy.types.PropertyGroup):
    addon_name: StringProperty(name="Addon name", default="My Scene",
                               description="Name of the v3DMM expansion")
    author: StringProperty(name="Author", default="", description="Author of the v3DMM expansion")
    scene_name: StringProperty(name="Scene name", default="ZZ-My Scene",
                               description="Name shown in 3DMM's background browser (default ones look like 'MY-Street Background')")
    bkgds_path: StringProperty(name="bkgds.3cn", subtype='FILE_PATH',
                               description="bkgds.3cn from your 3DMM install. The shared actor colors are read from it, "
                                           "and snds.3cn next to it provides the song list")
    output_dir: StringProperty(name="Output folder", subtype='DIR_PATH', default="~")
    scale: FloatProperty(name="Scale", default=1.0, min=0.0001,
                         description="3DMM units per Blender unit (world scale)")
    ground_z: FloatProperty(name="Ground height", default=0.0, unit='LENGTH',
                            description="Blender Z that becomes Y=0 (where actors stand) in 3DMM")
    camera_collection: PointerProperty(name="Collection", type=bpy.types.Collection,
                                       description="Only list cameras in this collection (empty = all cameras in the scene)")
    cams: CollectionProperty(type=DMMCamItem)
    cam_index: IntProperty()
    music: StringProperty(name="Music", default="Imaginopolis Theme", search=_song_search,
                          search_options={'SUGGESTION'},
                          description="A default 3DMM song (type to search), or the path to a .mid file to embed. '(none)' for silence")
    light_mode: EnumProperty(name="Lights", items=[
        ('LAMPS', "Scene lights", "Each Blender light (up to 2, brightest first) becomes a directional light in the scene"),
        ('STOCK', "3DMM default", "The light used in the Misty City background"),
    ], default='LAMPS')
    light_strength: FloatProperty(name="Light strength", default=1.0, min=0.0, soft_max=2.0)
    cno_base: IntProperty(name="Chunk number base", default=0, min=0,
                          description="0 = derived from the scene name. Set it to keep the same numbers when you rename a scene")
    dither: BoolProperty(name="Dither", default=True, description="Use Floyd-Steinberg dithering on the image")


# ---------------------------------------------------------------- camera list
def _scene_cameras(scene, st):
    objs = st.camera_collection.all_objects if st.camera_collection else scene.objects
    return sorted([o for o in objs if o.type == 'CAMERA'], key=lambda o: o.name)

def _sync_cams(scene):
    st = scene.dmm
    have = {o.name for o in _scene_cameras(scene, st)}
    for i in reversed(range(len(st.cams))):
        if st.cams[i].name not in have:
            st.cams.remove(i)
    known = {c.name for c in st.cams}
    for o in _scene_cameras(scene, st):
        if o.name not in known:
            it = st.cams.add(); it.name = o.name; it.use = True
    st.cam_index = min(st.cam_index, max(len(st.cams) - 1, 0))

def _ordered_cameras(scene):
    _sync_cams(scene)
    return [scene.objects[c.name] for c in scene.dmm.cams if c.use]


class DMM_OT_cam_refresh(bpy.types.Operator):
    bl_idname = "dmm.cam_refresh"; bl_label = "Refresh camera list"
    def execute(self, context):
        _sync_cams(context.scene); return {'FINISHED'}

class DMM_OT_cam_move(bpy.types.Operator):
    bl_idname = "dmm.cam_move"; bl_label = "Move camera"
    direction: EnumProperty(items=[('UP', "Up", ""), ('DOWN', "Down", ""), ('FIRST', "First", "")])
    def execute(self, context):
        st = context.scene.dmm; i = st.cam_index
        if not (0 <= i < len(st.cams)): return {'CANCELLED'}
        j = {'UP': i - 1, 'DOWN': i + 1, 'FIRST': 0}[self.direction]
        j = max(0, min(len(st.cams) - 1, j))
        st.cams.move(i, j); st.cam_index = j
        return {'FINISHED'}

class DMM_OT_pick_midi(bpy.types.Operator, ImportHelper):
    bl_idname = "dmm.pick_midi"; bl_label = "Choose MIDI file"
    filter_glob: StringProperty(default="*.mid;*.midi", options={'HIDDEN'})
    def execute(self, context):
        context.scene.dmm.music = self.filepath; return {'FINISHED'}

class DMM_UL_cams(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.prop(item, "use", text="", emboss=False, icon='CHECKBOX_HLT' if item.use else 'CHECKBOX_DEHLT')
        ob = context.scene.objects.get(item.name)
        first = index == next((k for k, c in enumerate(data.cams) if c.use), -1)
        row.label(text=item.name, icon='OUTLINER_OB_CAMERA' if ob else 'ERROR')
        if first and item.use: row.label(text="scene thumbnail", icon='IMAGE_DATA')


# ---------------------------------------------------------------- rendering
def _render_rgb(scene, cam, tmpdir):
    scene.camera = cam
    path = os.path.join(tmpdir, 'shot.png')
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    try:
        img.colorspace_settings.name = 'Non-Color'   # raw encoded values, as in the PNG
        buf = np.empty(W * H * 4, np.float32)
        img.pixels.foreach_get(buf)
    finally:
        bpy.data.images.remove(img)
    a = buf.reshape(H, W, 4)[::-1, :, :3]
    return np.clip(np.rint(a * 255), 0, 255).astype(np.uint8)

def _depth(scene, depsgraph, cam, scale, wm=None):
    mw = np.array(cam.matrix_world)
    R = mw[:3, :3] / np.linalg.norm(mw[:3, :3], axis=0)
    origin = Vector(mw[:3, 3].tolist())
    P = cam.calc_matrix_camera(depsgraph, x=W, y=H)
    ty, tx = 1.0 / P[1][1], 1.0 / P[0][0]
    xs = (2 * (np.arange(W) + 0.5) / W - 1) * tx
    ys = (1 - 2 * (np.arange(H) + 0.5) / H) * ty
    fwd = -R[:, 2]
    dirs = xs[None, :, None] * R[:, 0] + ys[:, None, None] * R[:, 1] - R[:, 2]
    dirs /= np.linalg.norm(dirs, axis=2, keepdims=True)
    rows = dirs.tolist()
    out = np.full((H, W), np.inf)
    o3 = mw[:3, 3]
    for j in range(H):
        rj = rows[j]
        for i in range(W):
            ok, loc, _n, _idx, _obj, _m = scene.ray_cast(depsgraph, origin, Vector(rj[i]))
            if ok:
                out[j, i] = ((loc[0] - o3[0]) * fwd[0] + (loc[1] - o3[1]) * fwd[1] + (loc[2] - o3[2]) * fwd[2]) * scale
        if wm and j % 20 == 0:
            wm.progress_update(j)
    return out, float(np.degrees(2 * np.arctan(ty)))


# ---------------------------------------------------------------- lights
def _basis_from_dir(d):
    """3x3 BRender rows (right, up, back) so that the light/camera travels along -back = d."""
    d = np.asarray(d, float); d /= np.linalg.norm(d)
    back = -d
    right = np.cross([0.0, 1.0, 0.0], back)
    if np.linalg.norm(right) < 1e-6: right = np.array([1.0, 0.0, 0.0])
    right /= np.linalg.norm(right)
    up = np.cross(back, right)
    return np.array([right, up, back])

def _lights(scene, st, aim_point):
    if st.light_mode == 'STOCK':
        return None
    lamps = [o for o in scene.objects if o.type == 'LIGHT']
    lamps.sort(key=lambda o: -o.data.energy)
    out = []
    for o in lamps[:2]:
        mw = np.array(o.matrix_world)
        if o.data.type == 'POINT':
            d = np.array(aim_point) - mw[:3, 3]
            if np.linalg.norm(d) < 1e-6: d = np.array([0, 0, -1.0])
            d3 = C.bl_to_3dmm_vec(d, point=False)
            R = _basis_from_dir(d3)
        else:
            R = C.camera_matrix(mw, 1.0, 0.0)[:3]
            R = np.array([r / np.linalg.norm(r) for r in R])
        out.append((R, float(st.light_strength), 1))
    return out or None


# ---------------------------------------------------------------- export
class DMM_OT_export(bpy.types.Operator):
    bl_idname = "dmm.export_vxp"
    bl_label = "Export v3DMM expansion (.vxp)"

    def execute(self, context):
        scene = context.scene; st = scene.dmm
        if not os.path.isfile(bpy.path.abspath(st.bkgds_path)):
            self.report({'ERROR'}, "Set the path to bkgds.3cn first"); return {'CANCELLED'}
        cams = _ordered_cameras(scene)
        if not cams:
            self.report({'ERROR'}, "No enabled cameras"); return {'CANCELLED'}
        for c in cams:
            if c.data.type != 'PERSP':
                self.report({'ERROR'}, "Camera %s is not perspective" % c.name); return {'CANCELLED'}
        # music
        music, midi = S.MUSIC_IMAGINOPOLIS, None
        m = st.music.strip()
        if not m or m == NO_MUSIC:
            music = None
        elif m.lower().endswith(('.mid', '.midi')) or os.path.isfile(bpy.path.abspath(m)):
            p = bpy.path.abspath(m)
            try:
                midi = (MIDI.midi_to_mids(open(p, 'rb').read()), os.path.splitext(os.path.basename(p))[0])
            except Exception as ex:
                self.report({'ERROR'}, "Can't use MIDI file: %s" % ex); return {'CANCELLED'}
        else:
            songs = S.stock_songs(_snds_path(st))
            hit = {k.lower(): v for k, v in songs.items()}.get(m.lower())
            if hit is None:
                self.report({'ERROR'}, "Unknown song '%s' (type to search the list)" % m); return {'CANCELLED'}
            music = (hit[0], 'MSND', hit[1])
        r = scene.render; saved = (r.resolution_x, r.resolution_y, r.resolution_percentage, r.pixel_aspect_x,
                                   r.pixel_aspect_y, r.filepath, r.image_settings.file_format,
                                   r.image_settings.color_mode, r.image_settings.color_depth, scene.camera, r.film_transparent)
        wm = context.window_manager
        spawns = [(p[0] * st.scale, 0.0, p[2] * st.scale) for p in
                  (C.bl_to_3dmm_vec(np.array(o.matrix_world.translation), 1.0, 0.0)
                   for o in sorted(scene.objects, key=lambda o: o.name)
                   if o.type == 'EMPTY' and o.name.lower().startswith('spawn'))]
        shots = []
        try:
            r.resolution_x, r.resolution_y, r.resolution_percentage = W, H, 100
            r.pixel_aspect_x = r.pixel_aspect_y = 1.0
            r.image_settings.file_format = 'PNG'; r.image_settings.color_mode = 'RGB'; r.image_settings.color_depth = '8'
            r.film_transparent = False
            with tempfile.TemporaryDirectory() as tmp:
                wm.progress_begin(0, H)
                for cam in cams:
                    rgb = _render_rgb(scene, cam, tmp)
                    dg = context.evaluated_depsgraph_get()
                    depth, fov = _depth(scene, dg, cam, st.scale, wm)
                    M = C.camera_matrix(np.array(cam.matrix_world), st.scale, st.ground_z)
                    if not spawns:
                        d = -M[2]; o = M[3]
                        pl = [(0.0, 0.0, 0.0)]
                        if d[1] < -1e-3:
                            t = -o[1] / d[1]; pl = [(o[0] + d[0] * t, 0.0, o[2] + d[2] * t)]
                    else:
                        pl = spawns
                    shots.append(E.Shot(M=M, fov_deg=fov, rgb=rgb, depth=depth, placements=pl))
                wm.progress_end()
        finally:
            (r.resolution_x, r.resolution_y, r.resolution_percentage, r.pixel_aspect_x, r.pixel_aspect_y, r.filepath,
             r.image_settings.file_format, r.image_settings.color_mode, r.image_settings.color_depth,
             scene.camera, r.film_transparent) = saved
        # light aim for point lamps: where the first camera's view meets the ground
        aim = np.array(cams[0].matrix_world.translation) + np.array(cams[0].matrix_world.to_3x3() @ Vector((0, 0, -10)))
        settings = E.Settings(name=st.scene_name, addon_name=st.addon_name, author=st.author,
                              bkgds_path=bpy.path.abspath(st.bkgds_path), music=music, midi=midi,
                              dither=st.dither, lights=_lights(scene, st, aim), cno_override=st.cno_base)
        path = E.export_vxp(shots, settings, bpy.path.abspath(st.output_dir), log=lambda m: print('[3DMM]', m))
        self.report({'INFO'}, "Wrote " + path)
        return {'FINISHED'}


# ---------------------------------------------------------------- UI
class DMM_PT_panel(bpy.types.Panel):
    bl_label = "3DMM Scene Maker"
    bl_idname = "3DMMSM_PT_panel"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "scene"

    def draw(self, context):
        l = self.layout; l.use_property_split = True; l.use_property_decorate = False
        st = context.scene.dmm
        col = l.column(heading="Expansion")
        col.prop(st, "addon_name"); col.prop(st, "author")
        l.prop(st, "scene_name")
        l.prop(st, "bkgds_path"); l.prop(st, "output_dir")

        b = l.box(); b.label(text="World")
        b.prop(st, "scale"); b.prop(st, "ground_z")
        b.label(text="Empties named 'Spawn1', 'Spawn2', etc. = actor spawn points", icon='INFO')

        b = l.box(); b.label(text="Camera angles (first one is used as the scene thumbnail)")
        b.prop(st, "camera_collection")
        row = b.row()
        row.template_list("DMM_UL_cams", "", st, "cams", st, "cam_index", rows=4)
        c = row.column(align=True)
        c.operator("dmm.cam_refresh", icon='FILE_REFRESH', text="")
        c.separator()
        c.operator("dmm.cam_move", icon='TRIA_UP', text="").direction = 'UP'
        c.operator("dmm.cam_move", icon='TRIA_DOWN', text="").direction = 'DOWN'
        c.operator("dmm.cam_move", icon='EVENT_F', text="").direction = 'FIRST'

        b = l.box(); b.label(text="Music")
        row = b.row(align=True); row.prop(st, "music", text="Song"); row.operator("dmm.pick_midi", icon='FILEBROWSER', text="")
        try:
            S.stock_songs(_snds_path(st))
            b.label(text="Default songs: " + ", ".join(S.last_song_notes), icon='CHECKMARK')
        except Exception as ex:
            b.label(text="Song list unavailable: %s" % ex, icon='ERROR')

        b = l.box(); b.label(text="Lighting")
        b.prop(st, "light_mode"); b.prop(st, "light_strength")

        col = l.column(); col.prop(st, "cno_base"); col.prop(st, "dither")
        l.operator("dmm.export_vxp", icon='EXPORT')


classes = (DMMCamItem, DMMSettings, DMM_OT_cam_refresh, DMM_OT_cam_move, DMM_OT_pick_midi, DMM_UL_cams,
           DMM_OT_export, DMM_PT_panel)

@bpy.app.handlers.persistent
def _on_depsgraph(*_):
    # keep the camera list in step with the scene (only writes when something changed)
    try:
        for scene in bpy.data.scenes:
            st = scene.dmm
            want = {o.name for o in _scene_cameras(scene, st)}
            if want != {c.name for c in st.cams}:
                _sync_cams(scene)
    except Exception:
        pass

def register():
    for c in classes: bpy.utils.register_class(c)
    bpy.types.Scene.dmm = PointerProperty(type=DMMSettings)
    bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph)

def unregister():
    if _on_depsgraph in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_depsgraph)
    del bpy.types.Scene.dmm
    for c in reversed(classes): bpy.utils.unregister_class(c)
