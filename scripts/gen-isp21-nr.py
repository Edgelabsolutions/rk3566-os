#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Kray-SoM-RKC: noise reduction and sharpening register tables for
# kray-cam-3a (RK3566 ISP v2.1), from a Rockchip rkaiq IQ tuning file.
#
# This is a Python port of the parts of Rockchip's camera_engine_rkaiq that
# turn tuning parameters into ISP register values for the ISP21 colour noise
# reduction (acnr, CNR v1), luma noise reduction (aynr2, YNR v2),
# sharpening (asharp3, sharp v3) and Bayer noise reduction (arawnr2,
# bayernr v2: 2D BAYNR and temporal BAY3D): cnr_init_params_json_V1,
# cnr_select_params_by_ISO_V1, cnr_fix_transfer_V1, ynr_init_params_json_V2,
# ynr_select_params_by_ISO_V2, ynr_fix_transfer_V2, sharp_init_params_json_V3,
# sharp_select_params_by_ISO_V3, sharp_fix_transfer_V3,
# bayernr2D/3D_init_params_json_V2, bayernr2D/3D_select_params_by_ISO_V2,
# bayernr2D/3D_fix_transfer_V2 and the field order of
# Isp21Params.cpp. Source: rkaiq 2024_04_08 as mirrored in
# https://github.com/JeffyCN/mirrors (branch rkaiq-2024_04_08).
# Copyright (c) 2019-2024 Rockchip Electronics Co., Ltd.
# Licensed under the Apache License, Version 2.0.
#
# Usage: gen-isp21-nr.py IQ.json OUT.conf [--setting N] [--denoise S]
#        [--sharpen S] [--note FILE]
#
# --denoise and --sharpen scale the strength as rkaiq's fStrength/fPercent
# do (1.0 = the tuning file). --noise FILE replaces the tuning file's luma
# noise curves (YNR sigma_curve samples) with a measured noise model of the camera
# (see userpatches/camera-tools/kray-cam-3a/noise-imx219.txt).
#
# The output lists, for a grid of ISO values (50 x 2^(k/4), 50 to 12800),
# the fields of the kernel structs isp21_cnr_cfg, isp21_ynr_cfg,
# isp21_sharp_cfg, isp21_baynr_cfg and isp21_bay3d_cfg in their declaration
# order. kray-cam-3a takes the line for
# the ISO nearest to the sensor gain (ISO 50 = gain 1).
import argparse
import json
import math


def clip(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def ftoi(v):
    """C (int) conversion of a float: truncation toward zero."""
    return int(v)


def round_f(v):
    """rkaiq ROUND_F: round half away from zero."""
    return int(math.floor(v + 0.5)) if v >= 0 else int(math.ceil(v - 0.5))


def interp(isos, values, iso):
    """Linear interpolation between ISO points, clamped at the ends."""
    if iso <= isos[0]:
        return values[0]
    if iso >= isos[-1]:
        return values[-1]
    for i in range(len(isos) - 1):
        if isos[i] <= iso <= isos[i + 1]:
            r = (iso - isos[i]) / (isos[i + 1] - isos[i])
            if isinstance(values[i], list):
                return [a + r * (b - a) for a, b in zip(values[i], values[i + 1])]
            return values[i] + r * (values[i + 1] - values[i])
    return values[-1]


def nearest(isos, values, iso):
    """The entry of the nearest ISO point (rkaiq's choice for on/off fields)."""
    for i, v in enumerate(isos):
        if iso < v:
            if i == 0:
                return values[0]
            return values[i] if iso - isos[i - 1] > v - iso else values[i - 1]
    return values[-1]


# ------------------------------------------------------------------ CNR v1

def cnr_fields(t, kernel, iso, strength=1.0):
    isos = [x["iso"] for x in t]
    g = lambda k: interp(isos, [x[k] for x in t], iso)
    # cnr_select_params_by_ISO_V1 takes the bypass flags and hf_bf_wgt_clip
    # from the ISO step at or below.
    idx = 0
    for i in range(len(isos) - 1):
        if isos[i] <= iso <= isos[i + 1]:
            idx = i
    if iso > isos[-1]:
        idx = len(isos) - 1
    s = max(strength, 1e-6)
    log2e_bits, sigma_fix, uvgain, sgm_ratio = 6, 8, 4, 4
    log2e = int(0.8493 * (1 << (log2e_bits + sigma_fix)))
    f = {}
    f["thumb_mix_cur_en"] = 0
    f["exgain_bypass"] = 1
    f["hq_bila_bypass"] = t[idx]["hf_bypass"]
    f["lq_bila_bypass"] = t[idx]["lf_bypass"]
    f["exgain_mux"] = clip(ftoi(g("cnr_exgain") * (1 << 4)), 0, 255)
    f["gain_iso"] = clip(ftoi(g("cnr_g_gain") * (1 << 7)), 0, 128)
    f["gain_offset"] = clip(ftoi(g("color_sat_adj_alpha") * (1 << 4)), 0, 16)
    f["gain_1sigma"] = clip(ftoi(g("color_sat_adj") * 1), 0, 255)
    f["gain_uvgain0"] = clip(ftoi(g("hf_color_sat") / s * (1 << uvgain)), 0, 127)
    f["gain_uvgain1"] = clip(ftoi(g("lf_color_sat") / s * (1 << uvgain)), 0, 127)
    f["lmed3_alpha"] = clip(ftoi(g("thumb_spikes_reducion_strength") * (1 << 4)), 0, 16)
    sigma_r2 = ftoi(log2e / g("thumb_denoise_strength") / s)
    uvgain2 = ftoi(g("thumb_color_sat") / s * (1 << uvgain))
    tmp_bit = 10 + log2e_bits + uvgain - 6
    sr = 1 << sgm_ratio
    ky = min(sr * (1 << uvgain) >> sgm_ratio, (1 << (4 + uvgain)) - 1)
    kuv = min(sr * uvgain2 >> sgm_ratio, (1 << (4 + uvgain)) - 1)
    f["lbf5_gain_y"] = clip((sigma_r2 * ky + (1 << (tmp_bit - 1))) >> tmp_bit, 0, 15)
    f["lbf5_gain_c"] = clip((sigma_r2 * kuv + (1 << (tmp_bit - 1))) >> tmp_bit, 0, 63)
    d = [clip(ftoi(k * (1 << 7)), 0, 128) for k in kernel]
    f["hmed3_alpha"] = clip(ftoi(g("hf_spikes_reducion_strength") * (1 << 4)), 0, 16)
    f["hbf5_sigma"] = clip(ftoi(log2e / g("hf_denoise_strength") / s), 0, (1 << 13) - 1)
    f["hbf5_weit_src"] = clip(ftoi(g("hf_denoise_alpha") * (1 << 7)), 0, 128)
    f["hbf5_min_wgt"] = clip(t[idx]["hf_bf_wgt_clip"], 0, 255)
    f["lbf3_sigma"] = clip(ftoi(log2e / g("lf_denoise_strength") / s), 0, (1 << 13) - 1)
    f["lbf5_weit_src"] = clip(ftoi(g("lf_denoise_alpha") * (1 << 7)), 0, 128)
    # isp21_cnr_cfg order
    return [f["thumb_mix_cur_en"], f["lq_bila_bypass"], f["hq_bila_bypass"], f["exgain_bypass"],
            f["exgain_mux"], f["gain_iso"], f["gain_offset"], f["gain_1sigma"],
            f["gain_uvgain1"], f["gain_uvgain0"], f["lmed3_alpha"], f["lbf5_gain_y"],
            f["lbf5_gain_c"], d[3], d[2], d[1], d[0], d[4], f["hmed3_alpha"],
            f["hbf5_weit_src"], f["hbf5_min_wgt"], f["hbf5_sigma"], f["lbf5_weit_src"],
            f["lbf3_sigma"]]


CNR_WIDTHS = [1] * 19 + [2] * 5


# ------------------------------------------------------------------ YNR v2

def load_noise(path):
    """Measured noise model: 'ymax = Y' and lines 'gain a b' (linear-light
    variance a * x + b, x = (Y / ymax)^2.2 for 8-bit output luma Y)."""
    ymax, rows = 255.0, []
    for line in open(path):
        line = line.split("#")[0].strip()
        if not line:
            continue
        if line.startswith("ymax"):
            ymax = float(line.split("=")[1])
            continue
        g, a, b = map(float, line.split())
        rows.append((g, a, b))
    rows.sort()
    return ymax, rows


def noise_ab(rows, gain):
    """a and b at a gain: log-log interpolation, the end segments extended."""
    gs = [r[0] for r in rows]
    i = 0
    while i < len(rows) - 2 and gain > gs[i + 1]:
        i += 1
    (g0, a0, b0), (g1, a1, b1) = rows[i], rows[i + 1]
    t = math.log(gain / g0) / math.log(g1 / g0)
    lerp = lambda u, v: math.exp(math.log(u) + t * (math.log(v) - math.log(u)))
    return lerp(a0, a1), lerp(max(b0, 1e-12), max(b1, 1e-12))


def noise_sigma(ymax, rows, iso):
    """YNR noise samples (12-bit sigma) at the 17 luma points (12-bit luma
    0, 256, ..., 4096) for an ISO (50 = gain 1). rkaiq evaluates its
    polynomial sigma_curve at the same points; the samples come straight
    from the model here, as a 4th-order polynomial does not follow the steep
    rise of the noise in the shadows after the gamma. Below 8-bit luma 16
    the samples stay at the value there: the gamma's slope goes to infinity
    at black, and the measurements start at 8."""
    a, b = noise_ab(rows, iso / 50.0)
    out = []
    for i in range(17):
        y12 = 4096.0 if i == 16 else i * 256.0
        y8 = clip(y12 / 16, 16.0, ymax - 1)
        x = (y8 / ymax) ** 2.2
        sigma8 = math.sqrt(max(a * x + b, 0)) * (ymax / 2.2) * x ** (1 / 2.2 - 1)
        out.append(sigma8 * 16)
    return out


def ynr_iso_params(calib, tuning, noise=None):
    """ynr_init_params_json_V2: noise curve samples per ISO."""
    out = []
    for c, t in zip(calib, tuning):
        luma, sigma = [], []
        for i in range(17):
            x = float(1 << 12) if i == 16 else float(i * (1 << (12 - 4)))
            sc = c["sigma_curve"]
            s = sc[0] * x ** 4 + sc[1] * x ** 3 + sc[2] * x ** 2 + sc[3] * x + sc[4]
            luma.append(int(x) >> 2)  # 12-bit calibration, 10-bit processing
            sigma.append(max(s, 0.0))
        if noise:
            sigma = noise_sigma(noise[0], noise[1], c["iso"])
        p = dict(t)
        p.update(iso=c["iso"], luma=luma, sigma=sigma, ci=[c["ynr_ci_l"], c["ynr_ci_h"]])
        out.append(p)
    return out


def ynr_fields(params, iso, width, height, strength=1.0):
    isos = [p["iso"] for p in params]
    cur = nearest(isos, params, iso)
    g = lambda k: interp(isos, [p[k] for p in params], iso)
    s = max(strength, 1e-6)
    f = {}
    f["thumb_mix_cur_en"] = 0
    f["global_gain_alpha"] = 8
    f["global_gain"] = 16
    f["flt1x1_bypass_sel"] = 0
    for k in ("sft5x5", "flt1x1", "lgft3x3", "lbft5x5", "bft3x3"):
        f[k + "_bypass"] = cur["ynr_%s_bypass" % k]
    r_sq_inv = 16.0 / (width * width + height * height)
    lg = math.log2(r_sq_inv)
    e = abs(math.ceil(lg))
    m = round_f(2 ** (lg + e) * 256)
    f["rnr_max_r"] = clip((m << 5) + e, 0, 0x3fff)
    f["low_bf_inv1"] = clip(ftoi(1.0 / g("low_bf_1") / s * (1 << 9)), 0, 0x3fff)
    f["low_bf_inv0"] = clip(ftoi(1.0 / g("low_bf_0") / s * (1 << 9)), 0, 0x3fff)
    f["low_peak_supress"] = clip(ftoi(g("low_peak_supress") / s * (1 << 7)), 0, 0x80)
    f["low_thred_adj"] = clip(ftoi(g("low_thred_adj") * (1 << 6)), 0, 0x7ff)
    f["low_dist_adj"] = clip(ftoi(g("low_dist_adj") * (1 << 2)), 0, 0x1ff)
    f["low_edge_adj_thresh"] = clip(ftoi(g("low_edge_adj_thresh")), 0, 0x3ff)
    f["low_bi_weight"] = clip(ftoi(g("low_bi_weight") * s * (1 << 7)), 0, 0x80)
    f["low_weight"] = clip(ftoi(g("low_weight") * s * (1 << 7)), 0, 0x80)
    f["low_center_weight"] = clip(ftoi(g("low_center_weight") / s * (1 << 10)), 0, 0x400)
    f["hi_min_adj"] = clip(ftoi(g("hi_min_adj") / s * (1 << 6)), 0, 0x3f)
    f["high_thred_adj"] = clip(ftoi(g("high_thred_adj") * s * (1 << 6)), 0, 0x7ff)
    f["high_retain_weight"] = clip((1 << 7) - ftoi(g("high_weight") * s * (1 << 7)), 0, 0x80)
    f["hi_edge_thed"] = clip(ftoi(g("hi_edge_thed") / s), 0, 0xff)
    w2 = int(g("base_filter_weight_2") * 64 / 2 + 0.5)
    w1 = int(g("base_filter_weight_1") * 64 / 2 + 0.5)
    w0 = 64 - w1 * 2 - w2 * 2
    bfw = [clip(w0, 0, 0x40), clip(w1, 0, 0x1f), clip(w2, 0, 0xf)]

    def gauss(sigma):
        c1 = math.exp(-1 / (2 * sigma * sigma))
        c0 = c1 * c1
        tot = 1 + 4 * c1 + 4 * c0
        a = int(c0 / tot * 256 + 0.5)
        b = int(c1 / tot * 256 + 0.5)
        return [clip(a, 0, 0x3f), clip(b, 0, 0x3f), clip(256 - a * 4 - b * 4, 0, 0x100)]
    g1 = gauss(g("low_filt_strength_0"))
    g2 = gauss(g("low_filt_strength_1"))
    dw = [clip(ftoi(v * (1 << 4)), 0, 0x10) for v in g("high_direction_weight")]
    luma = [clip(int(v), 0, 0x400) for v in g("luma")]
    sigma, ci = g("sigma"), g("ci")
    lsgm = [clip(ftoi(v * ci[0] * (1 << 3)), 0, 0xfff) for v in sigma]
    hsgm = [clip(ftoi(v * ci[1] * (1 << 3)), 0, 0xfff) for v in sigma]
    rnr = [clip(int(v * 16), 0, 0xff) for v in g("rnr_strength")]
    # isp21_ynr_cfg order
    return ([f["thumb_mix_cur_en"], f["global_gain_alpha"], f["global_gain"], f["flt1x1_bypass_sel"],
             f["sft5x5_bypass"], f["flt1x1_bypass"], f["lgft3x3_bypass"], f["lbft5x5_bypass"],
             f["bft3x3_bypass"], f["rnr_max_r"], f["low_bf_inv1"], f["low_bf_inv0"],
             f["low_peak_supress"], f["low_thred_adj"], f["low_dist_adj"], f["low_edge_adj_thresh"],
             f["low_bi_weight"], f["low_weight"], f["low_center_weight"], f["hi_min_adj"],
             f["high_thred_adj"], f["high_retain_weight"], f["hi_edge_thed"],
             bfw[2], bfw[1], bfw[0], g1[2], g1[1], g1[0], g2[2], g2[1], g2[0],
             dw[3], dw[2], dw[1], dw[0], dw[7], dw[6], dw[5], dw[4]]
            + luma + lsgm + hsgm + rnr)


YNR_WIDTHS = [1] * 9 + [2] * 12 + [1] * 5 + [2] * 6 + [1] * 8 + [2] * 51 + [1] * 17


# ---------------------------------------------------------------- sharp v3

def sharp_fields(t, iso, strength=1.0):
    isos = [x["iso"] for x in t]
    g = lambda k: interp(isos, [x[k] for x in t], iso)
    gl = lambda k: interp(isos, [x["luma_para"][k] for x in t], iso)
    gk = lambda k: interp(isos, [x["kernel_para"][k] for x in t], iso)
    p = max(strength, 1e-6)
    # sharp_init_params_json_V3 keeps the luma points of the last ISO step
    luma_point = t[-1]["luma_para"]["luma_point"]
    sharp_ratio = clip(round_f(g("sharp_ratio") * p * (1 << 2)), 0, 63)
    bf_ratio = clip(round_f(g("bf_ratio") / p * (1 << 7)), 0, 128)
    gaus_ratio = clip(round_f(g("gaus_ratio") / p * (1 << 7)), 0, 128)
    pbf_ratio = clip(round_f(g("pbf_ratio") / p * (1 << 7)), 0, 128)
    luma_dx = [clip(int(math.log(luma_point[i + 1] - luma_point[i]) / math.log(2)), 0, 15) for i in range(7)]
    luma_sigma = gl("luma_sigma")

    def sigma_inv(gain, add):
        vals = [math.floor((ls * gain + add) / p) for ls in luma_sigma]
        bits0 = math.floor(math.log(min(vals)) / math.log(2))
        bits2 = 9 + bits0
        inv = [clip(int(round_f(1.0 / (ls * gain + add) * p * (1 << bits2))), 0, 1023) for ls in luma_sigma]
        return inv, bits2 - 5
    pbf_inv, pbf_shift = sigma_inv(g("pbf_gain"), g("pbf_add"))
    bf_inv, bf_shift = sigma_inv(g("bf_gain"), g("bf_add"))
    # rkaiq swaps the two shifts; kept as is, the tuning was made with it
    pbf_sigma_shift = clip(bf_shift, 0, 15)
    bf_sigma_shift = clip(pbf_shift, 0, 15)
    ehf = [clip(ftoi(v * p), 0, 1023) for v in gl("local_sharp_strength")]
    clip_hf = [clip(ftoi(v * p), 0, 1023) for v in gl("hf_clip")]

    def coef(k):
        c = [clip(round_f(v * (1 << 7)), 0, 127) for v in gk(k)]
        c[0] = clip(c[0] + (1 << 7) - (c[0] + 4 * c[1] + 4 * c[2]), 0, 127)
        return c
    pbf_c, bf_c, gaus_c = coef("prefilter_coeff"), coef("GaussianFilter_coeff"), coef("hfBilateralFilter_coeff")
    # isp21_sharp_cfg order
    return ([0, sharp_ratio, bf_ratio, gaus_ratio, pbf_ratio] + luma_dx + pbf_inv + bf_inv
            + [bf_sigma_shift, pbf_sigma_shift] + ehf + clip_hf
            + [pbf_c[2], pbf_c[1], pbf_c[0], bf_c[2], bf_c[1], bf_c[0], gaus_c[2], gaus_c[1], gaus_c[0]])


SHARP_WIDTHS = [1] * 5 + [1] * 7 + [2] * 16 + [1] * 2 + [2] * 16 + [1] * 9


# ------------------------------------------------- Bayer NR v2 (2D and 3D)

def gain_interp(isos, values, iso):
    """bayernr*_select_params_by_ISO_V2: linear in the integer gain iso/50."""
    g = max(int(iso / 50), 1)
    gs = [i // 50 for i in isos]
    lo = hi = 0
    for i in range(len(gs) - 1):
        if gs[i] <= g <= gs[i + 1]:
            lo, hi = i, i + 1
    if g > gs[-1]:
        lo, hi = len(gs) - 2, len(gs) - 1
        g = gs[-1]
    if gs[hi] == gs[lo]:
        return values[lo]
    wl = (gs[hi] - g) / (gs[hi] - gs[lo])
    wh = (g - gs[lo]) / (gs[hi] - gs[lo])
    if isinstance(values[lo], list):
        return [wl * a + wh * b for a, b in zip(values[lo], values[hi])]
    return wl * values[lo] + wh * values[hi]


def baynr_fields(calib, t2d, iso, strength=1.0):
    s = max(strength, 1e-6)
    isos = [c["iso"] for c in calib]
    sigma = gain_interp(isos, [c["sigma"] for c in calib], iso)
    luma = calib[0]["lumapoint"]
    tisos = [x["iso"] for x in t2d]
    g = lambda k: gain_interp(tisos, [x[k] for x in t2d], iso)
    gauss_en = 1 if g("gauss_guide") != 0 else 0
    dgain0 = clip((1 << 10) // (1 << (10 - 8)), 0, 0xffff)   # one frame: gain 1.0
    pix_diff = clip((1 << 14) - 1, 0, 0x3fff)
    diff_thld = clip(1 << 10, 0, 0x3ff)
    softthld = clip(ftoi(g("ratio") / s * (1 << 10)), 0, 0x3ff)
    streng = clip(ftoi(g("filter_strength") * s * (1 << 8)), 0, 0xfff)
    reg_w1 = clip(ftoi(g("weight") * s * (1 << 10)), 0, 0x3ff)
    sx = [clip(int(v), 0, 0xffff) for v in luma]
    sy = [clip(ftoi(v), 0, 0xffff) for v in sigma]
    edgesofts = g("edgesofts")
    ypos, xpos = [4, 4, 4, 3, 3, 2, 2, 1], [4, 2, 0, 3, 1, 2, 0, 1]
    w = []
    for i in range(8):
        t1 = (ypos[i] ** 2 + xpos[i] ** 2) / (2 * edgesofts * edgesofts)
        w.append(int(math.exp(-t1) * (1 << 10)))
    weit = [clip(w[5], 0, 0x3ff), clip(w[6], 0, 0x3ff), clip(w[7], 0, 0x3ff)]
    # isp21_baynr_cfg order
    return ([gauss_en, 0, 0, dgain0, 0, pix_diff, diff_thld, softthld, streng, reg_w1]
            + sx + sy + [weit[2], weit[1], weit[0]])


BAYNR_WIDTHS = [1, 1] + [2] * 8 + [2] * 32 + [2] * 3


def bay3d_fields(calib, t3d, iso, strength=1.0):
    s = max(strength, 1e-6)
    isos = [c["iso"] for c in calib]
    sigma = gain_interp(isos, [c["sigma"] for c in calib], iso)
    luma = calib[0]["lumapoint"]
    tisos = [x["iso"] for x in t3d]
    g = lambda k: gain_interp(tisos, [x[k] for x in t3d], iso)
    softwgt = clip(ftoi(g("softwgt") / s * (1 << 10)), 0, 0x3ff)
    sigratio = clip(int(1.5 * (1 << 10)), 0, 0x3fff)
    glbpk2 = 1024
    exp_str = clip(ftoi(g("filter_strength") / s * (1 << 10)), 0, 0x3ff)
    stren = clip(int(1.0 * (1 << 8)), 0, 0x100)
    wgtlmt_h = clip(ftoi((1 - g("hi_clipwgt")) * s * (1 << 10)), 0, 0x3ff)
    wgtlmt_l = clip(ftoi((1 - g("lo_clipwgt")) * s * (1 << 10)), 0, 0x3ff)
    sx = [clip(int(v), 0, 0xffff) for v in luma]
    sy = [clip(ftoi(v), 0, 0xffff) for v in sigma]
    # isp21_bay3d_cfg order: exp_sel 1, bypass_en 0, pk_en 1 (local peak)
    return [1, 0, 1, softwgt, sigratio, glbpk2, exp_str, stren, wgtlmt_h, wgtlmt_l] + sx + sy


BAY3D_WIDTHS = [1, 1, 1, 2, 2, 4, 2, 2, 2, 2] + [2] * 32


def main():
    ap = argparse.ArgumentParser(description="ISP21 NR/sharpening tables for kray-cam-3a")
    ap.add_argument("iq")
    ap.add_argument("out")
    ap.add_argument("--setting", type=int, default=0, help="IQ setting index (0 = LSNR, linear)")
    ap.add_argument("--width", type=int, default=3280)
    ap.add_argument("--height", type=int, default=2464)
    ap.add_argument("--denoise", type=float, default=1.0, help="noise reduction strength")
    ap.add_argument("--sharpen", type=float, default=1.0, help="sharpening strength")
    ap.add_argument("--bayer", type=float, default=1.0, help="Bayer 2D noise reduction strength")
    ap.add_argument("--temporal", type=float, default=1.0, help="Bayer temporal noise reduction strength")
    ap.add_argument("--noise", help="measured noise model for the YNR noise curves")
    ap.add_argument("--note", help="file with the header comment for the output")
    a = ap.parse_args()
    scene = json.load(open(a.iq))["main_scene"][0]["sub_scene"][0]["scene_isp21"]
    cnr = scene["cnr_v1"]["TuningPara"]
    cnr_t = cnr["Setting"][a.setting]["Tuning_ISO"]
    ynr = scene["ynr_v2"]
    ynr_p = ynr_iso_params(ynr["CalibPara"]["Setting"][a.setting]["Calib_ISO"],
                           ynr["TuningPara"]["Setting"][a.setting]["Tuning_ISO"],
                           load_noise(a.noise) if a.noise else None)
    sharp_t = scene["sharp_v3"]["TuningPara"]["Setting"][a.setting]["Tuning_ISO"]
    bnr = scene["bayernr_v2"]
    bnr_c = bnr["CalibPara"]["Setting"][a.setting]["Calib_ISO"]
    bnr_2d = bnr["Bayernr2D"]["Setting"][a.setting]["Tuning_ISO"]
    bnr_3d = bnr["Bayernr3D"]["Setting"][a.setting]["Tuning_ISO"]
    lines = []
    for k in range(33):
        iso = round(50 * 2 ** (k / 4))
        for name, vals, widths in (
                ("cnr", cnr_fields(cnr_t, cnr["Kernel_Coeff"]["kernel_5x5"], iso, a.denoise), CNR_WIDTHS),
                ("ynr", ynr_fields(ynr_p, iso, a.width, a.height, a.denoise), YNR_WIDTHS),
                ("sharp", sharp_fields(sharp_t, iso, a.sharpen), SHARP_WIDTHS),
                ("baynr", baynr_fields(bnr_c, bnr_2d, iso, a.bayer), BAYNR_WIDTHS),
                ("bay3d", bay3d_fields(bnr_c, bnr_3d, iso, a.temporal), BAY3D_WIDTHS)):
            assert len(vals) == len(widths), (name, len(vals), len(widths))
            for v, w in zip(vals, widths):
                assert 0 <= v < (1 << (8 * w)), (name, iso, v, w)
            lines.append("%s %d = %s" % (name, iso, " ".join(map(str, vals))))
    with open(a.out, "w") as f:
        if a.note:
            f.write(open(a.note).read())
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
