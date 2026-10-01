#!/usr/bin/env python3
"""Make an rkaiq (ISP v2.1) tuning file for the Sony IMX477 from another sensor's.

There is no Rockchip tuning (IQ) file for the IMX477. This takes the OV13850
one shipped with rkaiq and replaces what describes the sensor itself, so the
auto exposure drives the IMX477 correctly:

  - analogue gain: the IMX477 register is 1024 - 1024 / gain (gain 1 to 22.26,
    register 0 to 978); rkaiq computes register = C1 * gain^M0 - C0, which
    gives that exactly with C1 = C0 = -1024 and M0 = -1;
  - exposure: in lines, at least 4, at most frame length - 22;
  - no sensor digital gain, no HDR;
  - resolution: the 2028x1520 binned mode (the driver's default);
  - lens shading correction off: the OV13850's tables belong to another lens.

Colour (AWB light sources, CCM, LSC) stays the OV13850's: auto white balance
and exposure work, but colours are not calibrated for the IMX477.

usage: make-imx477-iq.py <ov13850 isp21 json> <output json>
"""
import json
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src) as f:
    iq = json.load(f)

cal = iq["sensor_calib"]
cal["resolution"] = {"width": 2028, "height": 1520}
cal["Gain2Reg"] = {
    "GainMode": "EXPGAIN_MODE_LINEAR",
    # min gain, max gain, C1, C0, M0, min register, max register
    "GainRange": [1, 22.26, -1024, -1024, -1, 0, 978],
    "GainRange_len": 7,
}
cal["CISGainSet"]["CISAgainRange"] = {"Min": 1, "Max": 22.26}
cal["CISGainSet"]["CISExtraAgainRange"] = {"Min": 1, "Max": 1}
cal["CISGainSet"]["CISDgainRange"] = {"Min": 1, "Max": 1}
lin = cal["CISTimeSet"]["Linear"]
lin["CISTimeRegMin"] = 4
lin["CISLinTimeRegMaxFac"] = {"fCoeff": [1, 22]}
lin["CISTimeRegOdevity"] = {"fCoeff": [1, 0]}
cal["CISHdrSet"]["hdr_en"] = 0
cal["CISDcgSet"]["Linear"]["support_en"] = 0
cal["CISFlip"] = 0

for scene in iq["main_scene"]:
    for sub in scene["sub_scene"]:
        sub["scene_isp21"]["lsc_v2"]["common"]["enable"] = 0

with open(dst, "w") as f:
    json.dump(iq, f, indent=1)
