# framesconv

This is a case study of GPU-accelerated RGB-to-NV12 colorspace conversion by
means of OpenGL ES 3.1 compute shader. It also showcases following topics:
* GBM buffers allocation, reading and writing,
* Surfaceless EGL platform (EGL_MESA_platform_surfaceless),
* Surfaceless EGL context (EGL_KHR_surfaceless_context),
* Configless EGL context (EGL_KHR_no_config_context),
* Creating EGL image from dma_buf (EGL_EXT_image_dma_buf_import),
* Creating GL textures from EGL image (GL_OES_EGL_image).

The implementation allocates source and destination GBM buffers. Source GBM
buffer is filled with raw RGBX data from a provided file. Compute shader
converts RGB to NV12 writing output to the destination GBM buffer. In the end,
output GBM buffer is drained to a provided file.

## Building on Linux

framesconv depends on gbm, egl and glesv2. Once you have it installed, just
```
make
```

## Building anywhere else

I don't care about any other platforms except Linux, so you are on your own.
It's unlikely it would work at all, because EGL_EXT_image_dma_buf_import is
Linux-specific extension.

## Running

The commandline is
```
framesconv [-i input] -w width -h height [-o output] [-r render_node]
```

where
* `input` is either a) path to a source image, or b) `-` to read the data from
  standard input. In any case it's your responsibility to provide appropriate
  amount of input data. Source image is expected in raw 4-bytes RGBX format.
* `width` is width of the source image in pixels.
* `height` is height of the source image in pixels.
* `output` is either a) path to a destination image, or b) `-` to write the data
  to the standard output. Destination image is written in raw NV12 format.
* `render_node` is a path to the DRM render node.

Default value for `-i` is `-` making it to read from the standard input. Default
value for `-o` is `-` making it write to the standard output. Default value
`render_node` is `/dev/dri/renderD128`.

## Usage

Just provide a proper commandline, i.e.:
```
./framesconv -i lenna.rgb -w 512 -h 512 -o /tmp/lenna.yuv
Colorspace conversion took 4 milliseconds
```

## Bugs

Yes.
