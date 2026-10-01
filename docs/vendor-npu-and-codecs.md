# NPU and hardware video codecs on the vendor image

This guide covers the **vendor** image (Rockchip BSP kernel 6.1). The NPU and the full set of hardware encoders are only available there; the mainline image has hardware decoding only (see the README).

- [NPU](#npu)
  - [Check the NPU](#check-the-npu)
  - [Convert a model](#convert-a-model)
  - [Run a model from C](#run-a-model-from-c)
  - [Run a model from Python](#run-a-model-from-python)
  - [NPU frequency and load](#npu-frequency-and-load)
- [Hardware video codecs](#hardware-video-codecs)
  - [What the hardware supports](#what-the-hardware-supports)
  - [FFmpeg](#ffmpeg)
  - [GStreamer](#gstreamer)
  - [MPP test tools](#mpp-test-tools)
  - [Troubleshooting](#troubleshooting)

## NPU

The RK3566 NPU delivers about 0.8 TOPS on INT8. It runs models in Rockchip's `.rknn` format through the RKNN runtime, which the image ships preinstalled:

| Item | Location |
|---|---|
| Kernel driver | `rknpu` 0.9.8 (built into the vendor kernel) |
| Runtime library | `/usr/lib/librknnrt.so` (RKNN runtime 2.3.2) |
| C headers | `/usr/include/rknn_api.h`, `rknn_matmul_api.h`, `rknn_custom_op.h` |
| Smoke test | `/usr/share/kray-som-rkc/npu-test/` (source in `npu_test.c`) |

Models must be converted for the NPU on a PC with the same toolkit version (2.3.2) as the runtime on the board.

### Check the NPU

```bash
sudo cat /sys/kernel/debug/rknpu/version     # RKNPU driver: v0.9.8
ls /dev/dri/                                  # renderD129 is the NPU
sudo /usr/share/kray-som-rkc/npu-test/run.sh
```

The smoke test classifies two images with MobileNet v1. Expected: dog → class 156 (~0.93), cat → classes 278–286, about 4.5 ms per frame.

### Convert a model

Conversion runs on an x86_64 Linux PC (or WSL2) with [rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) **2.3.2**, matching the runtime on the board. It needs Python 3.8–3.12:

```bash
python3 -m venv rknn && . rknn/bin/activate
pip install rknn-toolkit2==2.3.2
```

```python
from rknn.api import RKNN

rknn = RKNN()
rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]],
            target_platform='rk3566')
rknn.load_onnx(model='model.onnx')
# dataset.txt lists a few dozen representative input images, one path per line
rknn.build(do_quantization=True, dataset='dataset.txt')
rknn.export_rknn('model.rknn')
rknn.release()
```

- `target_platform` must be `rk3566` (an `rk3568` model also runs on RK3566).
- Quantize to INT8 (`do_quantization=True`): that is what the NPU is fast at. FP16 models run, but much slower.
- `mean_values` / `std_values` move input normalization onto the NPU, so the application can feed raw `uint8` RGB.
- Ready-made examples for YOLO, ResNet, MobileNet, PP-OCR and others are in [rknn_model_zoo](https://github.com/airockchip/rknn_model_zoo).

Copy `model.rknn` to the board.

### Run a model from C

The runtime and headers are already installed, so applications build directly on the board:

```bash
sudo apt install -y gcc
gcc -O2 -o app app.c -lrknnrt
```

The minimal sequence (see `/usr/share/kray-som-rkc/npu-test/npu_test.c` for a complete program):

```c
#include <rknn_api.h>

rknn_context ctx;
rknn_init(&ctx, model_data, model_size, 0, NULL);

rknn_input in = {
	.index = 0,
	.type = RKNN_TENSOR_UINT8,
	.fmt = RKNN_TENSOR_NHWC,
	.size = width * height * 3,
	.buf = rgb_pixels,
};
rknn_inputs_set(ctx, 1, &in);
rknn_run(ctx, NULL);

rknn_output out = { .want_float = 1 };
rknn_outputs_get(ctx, 1, &out, NULL);
/* out.buf holds out.size bytes of float results */
rknn_outputs_release(ctx, 1, &out);
rknn_destroy(ctx);
```

Query the model's inputs and outputs with `rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, ...)` / `RKNN_QUERY_OUTPUT_ATTR` instead of hard-coding shapes.

### Run a model from Python

`rknn-toolkit-lite2` (the on-board inference API) is on PyPI for aarch64, but only for Python 3.7–3.12, while Debian 13 ships Python 3.13. Use a separate Python 3.12 environment, for example with [uv](https://docs.astral.sh/uv/):

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
uv venv -p 3.12 ~/rknn-lite && . ~/rknn-lite/bin/activate
uv pip install rknn-toolkit-lite2==2.3.2 numpy opencv-python-headless
```

```python
from rknnlite.api import RKNNLite
import cv2

rknn = RKNNLite()
rknn.load_rknn('model.rknn')
rknn.init_runtime()
img = cv2.cvtColor(cv2.imread('input.jpg'), cv2.COLOR_BGR2RGB)
outputs = rknn.inference(inputs=[img[None, ...]])
rknn.release()
```

`rknn-toolkit-lite2` uses the system `/usr/lib/librknnrt.so`; the wheel version must match it (2.3.2).

### NPU frequency and load

The NPU scales its clock with load. For stable latency, pin it at the highest available frequency:

```bash
cat /sys/class/devfreq/fde40000.npu/available_frequencies
echo performance | sudo tee /sys/class/devfreq/fde40000.npu/governor
sudo cat /sys/kernel/debug/rknpu/load        # "NPU load: xx%" while a model runs
```

The first `rknn_init` after boot is slower (model and library loading, NPU clock ramp-up); measure latency after a few warm-up runs.

## Hardware video codecs

The vendor image ships the Rockchip MPP stack, built by `scripts/build-multimedia.sh`:

| Package | What it provides |
|---|---|
| `rockchip-mpp` | `librockchip_mpp`, headers, `mpi_dec_test`, `mpi_enc_test`, ... |
| `librga-rockchip` | `librga` (RGA 2D engine: scaling, colour conversion, rotation) |
| `ffmpeg-rockchip` | `ffmpeg` and `ffprobe` in `/usr/local/bin` with `*_rkmpp` codecs and RGA filters |
| `gstreamer1.0-rockchip` | GStreamer elements `mppvideodec`, `mpph264enc`, `mpph265enc`, `mppjpegenc`, `mppjpegdec` |

### What the hardware supports

| | Decode | Encode |
|---|---|---|
| H.264 | up to 4K, 8-bit 4:2:0 | up to 1080p60 |
| H.265 / HEVC | up to 4K, 8/10-bit 4:2:0 | up to 1080p60 |
| VP9 | up to 4K, profile 0 and 2 | — |
| VP8, MPEG-1/2/4, H.263 | up to 1080p | — |
| MJPEG / JPEG | yes | yes |
| AV1 | — (listed by FFmpeg, but not present in RK3566) | — |

The hardware only handles 4:2:0 chroma. Streams encoded in 4:2:2 or 4:4:4 (for example High 4:4:4 H.264 from `x264` without `-pix_fmt yuv420p`) fail to decode in hardware.

Measured on the board with 1080p: H.264 decode ~270 fps in FFmpeg and ~450 fps in GStreamer; H.265 decode ~535 fps in GStreamer (300 frames in 0.56 s); H.264 → H.264 transcode ~67 fps and H.264 → H.265 ~68 fps (Kray-SoM-RKC, vendor 6.1.172).

### FFmpeg

`ffmpeg-rockchip` lives in `/usr/local/bin`, which comes first in `PATH`. Check the available hardware codecs:

```bash
ffmpeg -hide_banner -decoders | grep rkmpp
ffmpeg -hide_banner -encoders | grep rkmpp
ffmpeg -hide_banner -h encoder=h264_rkmpp      # all encoder options
```

**Decode** (benchmark, output discarded):

```bash
ffmpeg -c:v h264_rkmpp -i in.mp4 -f null -
ffmpeg -c:v hevc_rkmpp -i in.mkv -f null -
```

**Encode** from a raw source (camera, test pattern) at 4 Mbit/s constant bitrate:

```bash
ffmpeg -f v4l2 -video_size 1280x720 -i /dev/video0 \
       -c:v h264_rkmpp -rc_mode CBR -b:v 4M -g 60 out.mp4
```

**Transcode** H.264 → H.265 with both steps on the VPU, keeping the audio:

```bash
ffmpeg -c:v h264_rkmpp -i in.mp4 -c:v hevc_rkmpp -b:v 3M -c:a copy out.mkv
```

**Zero-copy transcode with RGA scaling**: frames stay in DMA buffers from the decoder through the RGA scaler to the encoder, so the CPU never touches the pixels:

```bash
ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -afbc rga -i in.mp4 \
       -vf scale_rkrga=w=1280:h=720:format=nv12 \
       -c:v h264_rkmpp -rc_mode VBR -b:v 2M -c:a copy out_720p.mp4
```

Useful encoder options (`h264_rkmpp`, `hevc_rkmpp`):

| Option | Values |
|---|---|
| `-rc_mode` | `VBR`, `CBR`, `CQP`, `AVBR` |
| `-b:v`, `-maxrate` | bitrate for `VBR` / `CBR` / `AVBR` |
| `-qp_init`, `-qp_min`, `-qp_max` | quantizer for `CQP` or as limits |
| `-g` | GOP length (keyframe interval) in frames |
| `-profile` (H.264) | `baseline`, `main`, `high` |
| `-level` | e.g. `4.1`, `5.1` |
| `-coder` (H.264) | `cabac` (default), `cavlc` |

RGA filters: `scale_rkrga` (resize / format conversion), `vpp_rkrga` (scale, crop, transpose), `overlay_rkrga` (compose two streams). See `ffmpeg -h filter=scale_rkrga`.

### GStreamer

List the Rockchip elements and their properties:

```bash
gst-inspect-1.0 rockchipmpp
gst-inspect-1.0 mpph264enc
```

**Decode** a file (benchmark):

```bash
gst-launch-1.0 filesrc location=in.mp4 ! qtdemux ! h264parse ! mppvideodec ! fakesink sync=false
```

**Play** on the HDMI screen from the console (no desktop needed):

```bash
gst-launch-1.0 filesrc location=in.mp4 ! qtdemux ! h264parse ! mppvideodec ! kmssink
```

**Record a USB camera** to H.264:

```bash
gst-launch-1.0 -e v4l2src device=/dev/video0 ! video/x-raw,width=1280,height=720,framerate=30/1 \
    ! videoconvert ! video/x-raw,format=NV12 \
    ! mpph264enc bps=4000000 rc-mode=cbr gop=60 ! h264parse ! mp4mux ! filesink location=cam.mp4
```

**Stream** the camera as RTP/H.264 over UDP to a PC at 192.168.1.10:

```bash
gst-launch-1.0 v4l2src device=/dev/video0 ! video/x-raw,width=1280,height=720,framerate=30/1 \
    ! videoconvert ! video/x-raw,format=NV12 \
    ! mpph264enc bps=2000000 rc-mode=cbr gop=30 ! h264parse config-interval=-1 \
    ! rtph264pay ! udpsink host=192.168.1.10 port=5000
```

Encoder properties (`mpph264enc`, `mpph265enc`): `bps` (bit/s), `bps-min`, `bps-max`, `rc-mode` (`cbr`, `vbr`, `fixqp`), `gop`, `qp-init`, `qp-min`, `qp-max`, `profile`, `level`, `rotation`, `width` / `height` (scale on the fly).

### MPP test tools

`rockchip-mpp` includes the Rockchip test programs, handy to check the hardware without FFmpeg or GStreamer. They take raw elementary streams:

```bash
ffmpeg -i in.mp4 -c:v copy -bsf:v h264_mp4toannexb -an in.h264
mpi_dec_test -i in.h264 -t 7          # -t 7 = H.264, 16777220 = H.265, 10 = VP9
mpi_enc_test -w 1280 -h 720 -t 7 -o out.h264 -n 300   # encodes a generated test pattern
```

Run them without arguments for the full option list. MPP writes its log to the kernel log and the journal rather than the terminal.

### Troubleshooting

- **`Driver does not support the selected stream` / decoder errors** — check the stream's pixel format: `ffprobe in.mp4` must show `yuv420p` (or `yuv420p10le` for HEVC/VP9).
- **`no element "h264parse"`** — install `gstreamer1.0-plugins-bad` (preinstalled from v1.3).
- **FFmpeg picks the software decoder** — pass `-c:v h264_rkmpp` (or `hevc_rkmpp`, `vp9_rkmpp`) *before* `-i`.
- **Debian's `ffmpeg` package** installs to `/usr/bin` and has no Rockchip support; the hardware-enabled one is `/usr/local/bin/ffmpeg`.
