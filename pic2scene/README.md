Transforms a picture (+ a depth map) into a 3DMM scene with a single camera angle.

## Usage

`python3 pic2scene.py image.png --depth image_depth.png --name "My Scene" --author "Me"`

(Simple)


`python3 pic2scene.py image.png --depth image_depth.exr --depth-scale 7 --fov (value) --auto-camera --music "Bistro Good Theme" --name "My Scene" --author "Me"`

(Recommended)

## Depth map

Generate a picture's depth map with Microsoft's MoGe: https://github.com/microsoft/moge

Run it locally and use `--estimate`

Or run for free here: https://huggingface.co/spaces/Ruicheng/MoGe-3

Replace `(value)` in the `--fov` option with the vertical FOV output.

You *may* need OpenEXR:

`pip install OpenEXR`

Use `-h` for help and see other options.
