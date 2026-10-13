"""Local, dependency-free HTML/PNG presentation of measured challenge results."""

from __future__ import annotations

import html
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from scene_model import direction_vector, project
from stress_scenarios import FAMILIES


def font(size: int):
    candidate = Path("C:/Windows/Fonts/msyh.ttc")
    return ImageFont.truetype(str(candidate), size) if candidate.exists() else ImageFont.load_default(size=size)


def percent(value) -> str:
    return "—" if value is None else f"{100 * value:.1f}%"


def number(value) -> str:
    return "—" if value is None else f"{value:.3f}"


def heat_colors(heat: np.ndarray) -> np.ndarray:
    # Display-only color map. Neither the map nor this resize enters inference.
    x = np.clip(heat, 0, 1)
    return np.uint8(255 * np.stack([np.clip(1.5 - np.abs(4 * x - k), 0, 1)
                                   for k in (3, 2, 1)], axis=-1))


def case_picture(case_dir: Path, destination: Path, title: str) -> None:
    scene = json.loads((case_dir / "truth.json").read_text(encoding="utf-8"))
    prediction = json.loads((case_dir / "predictions.json").read_text(encoding="utf-8"))
    with np.load(case_dir / "input.npz", allow_pickle=False) as data:
        rgb, heat = data["rgb"], data["heat"]
    srp = json.loads((case_dir / "srp.json").read_text(encoding="utf-8"))
    base = Image.fromarray(rgb).resize((640, 384), Image.Resampling.NEAREST)
    colored = Image.fromarray(heat_colors(heat)).resize((640, 384), Image.Resampling.BILINEAR)
    overlay = Image.blend(base, colored, .45)
    panels = [base.copy(), overlay, base.copy(), base.copy()]
    for index, panel in enumerate(panels):
        painter = ImageDraw.Draw(panel)
        # Truth shown only in report panels, never in saved model input RGB.
        for obj in scene["objects"]:
            u, v = obj["point_uv"]
            x, y = int(u * 640), int(v * 384)
            if obj["active"]:
                painter.ellipse((x - 7, y - 7, x + 7, y + 7), outline="#33ff77", width=2)
                painter.text((x + 9, y - 12), f"T{obj['source_id']}", font=font(16), fill="#33ff77", stroke_width=1, stroke_fill="#152018")
        sources = prediction["raw_fusion"]["sources"] if index == 2 else srp["sources"] if index == 3 else []
        for n, source in enumerate(sources):
            if not source.get("trusted", True):
                continue
            uv = source.get("point_uv")
            if uv is None:
                uv = project(direction_vector(source["azimuth_deg"], source["elevation_deg"]), scene["camera"])
            x, y = int(uv[0] * 640), int(uv[1] * 384)
            color = "#45dfff" if index == 2 else "#ffd55e"
            painter.line((x - 6, y, x + 6, y), fill=color, width=2)
            painter.line((x, y - 6, x, y + 6), fill=color, width=2)
            painter.text((x + 9, y + 6), f"{'Y' if index == 2 else 'S'}{n+1}", font=font(16), fill=color, stroke_width=1, stroke_fill="#152018")
    canvas = Image.new("RGB", (1312, 906), "#101c28")
    draw = ImageDraw.Draw(canvas)
    draw.text((16, 10), title, font=font(20), fill="white")
    draw.text((16, 40), "绿圈 T：真值；青十字 Y：神经源点；黄十字 S：SRP。可见物体不一定发声。", font=font(17), fill="#c2d2df")
    labels = ["视频 / 真值（绿圈仅用于此报告）", "真实 SRP 响应 / 真值", "冻结融合模型 / 真值", "SRP 连续峰值 / 真值"]
    for i, panel in enumerate(panels):
        x, y = 12 + (i % 2) * 652, 96 + (i // 2) * 408
        canvas.paste(panel, (x, y))
        draw.text((x, y - 26), labels[i], font=font(18), fill="white")
    raw = prediction["scores"]["raw_fusion"]
    draw.text((16, 884), f"模型 TP/FP/FN={raw['tp']}/{raw['fp']}/{raw['fn']}；上线门控：{prediction['gate_reason'] or '允许（仅检查静态可观测条件）'}", font=font(15), fill="#c2d2df")
    canvas.save(destination)


def overview_picture(report: dict, destination: Path) -> None:
    canvas = Image.new("RGB", (1200, 170 + 56 * len(FAMILIES)), "#101c28")
    draw = ImageDraw.Draw(canvas)
    draw.text((30, 20), "M128 冻结模型 · 新场景压力测试", font=font(28), fill="white")
    draw.text((30, 65), "柱形为召回率；每组是预定挑战，不代表真实环境出现概率。青：模型 / 黄：SRP", font=font(17), fill="#c2d2df")
    draw.text((815, 108), "模型精确率", font=font(17), fill="#c2d2df")
    draw.text((1000, 108), "模型误报 / 漏检", font=font(17), fill="#c2d2df")
    for i, (key, group) in enumerate(report["families"].items()):
        y = 147 + i * 56
        a, b = group["metrics"]["raw_fusion"], group["metrics"]["srp"]
        draw.text((30, y), FAMILIES[key][0], font=font(18), fill="white")
        for row, stats, color in ((0, a, "#45dfff"), (1, b, "#ffd55e")):
            yy = y + row * 19
            draw.rectangle((300, yy, 680, yy + 13), fill="#243748")
            recall = stats["recall"] or 0
            if recall:
                draw.rectangle((300, yy, 300 + int(380 * recall), yy + 13), fill=color)
            draw.text((700, yy - 3), percent(stats["recall"]), font=font(15), fill=color)
        draw.text((825, y), percent(a["precision"]), font=font(18), fill="white")
        draw.text((1030, y), f"{a['fp']} / {a['fn']}", font=font(18), fill="white")
    canvas.save(destination)


def write_report(report: dict, output: Path) -> None:
    assets = output / "report-assets"
    assets.mkdir(exist_ok=False)
    overview_picture(report, assets / "overview.png")
    e = html.escape
    sections = []
    for family, group in report["families"].items():
        rows = []
        for level in group["levels"]:
            a, b = level["metrics"]["raw_fusion"], level["metrics"]["srp"]
            near = f"{level['near_successes']}/{a['near_pair_scenes']}" if a["near_pair_scenes"] else "—"
            interval = level["near_wilson95"]
            if interval is not None:
                near += f" ({percent(interval[0])}–{percent(interval[1])})"
            rows.append(f"<tr><td>{e(level['label'])}</td><td>{a['scenes']}</td><td>{percent(a['precision'])}</td>"
                        f"<td>{percent(a['recall'])}</td><td>{percent(b['recall'])}</td><td>{a['fp']} / {a['fn']}</td>"
                        f"<td>{number(a['matched_angle_p95_deg'])}</td><td>{near}</td>"
                        f"<td>{percent(level['weak_source_recall'])}</td><td>{level['gated_cases']}</td></tr>")
        cards = []
        cases = group["case_ids"]
        selections = [("预定首例", cases[0])]
        for label, predicate in (("首个漏检", lambda r: r["fn"] > 0),
                                 ("首个误报", lambda r: r["fp"] > 0),
                                 ("首个完整正确", lambda r: r["complete_resolved"] and r["true_count"] > 0)):
            candidate = next((sid for sid in cases if predicate(report["cases"][sid]["scores"]["raw_fusion"])), None)
            if candidate and candidate not in [sid for _, sid in selections]:
                selections.append((label, candidate))
        for label, sid in selections:
            record = report["cases"][sid]
            title = f"{label} · {FAMILIES[family][0]} · {record['spec']['value_label']} · {sid}"
            picture = assets / f"{sid}.png"
            case_picture(output / "scenes" / sid, picture, title)
            cards.append(f'<details><summary>{e(title)}</summary><img loading="lazy" src="report-assets/{sid}.png" alt="{e(title)}">'
                         f'<a href="scenes/{sid}/predictions.json">逐项预测</a> · <a href="scenes/{sid}/truth.json">物理真值</a></details>')
        sections.append(f"<section><h2>{e(FAMILIES[family][0])}</h2><div class='scroll'><table><thead><tr>"
                        "<th>条件</th><th>样本</th><th>模型精确率</th><th>模型召回</th><th>SRP召回</th><th>模型误报/漏检</th>"
                        "<th>匹配角误差P95/°</th><th>严格近邻通过（95%区间）</th><th>弱源召回</th><th>门控拦截</th>"
                        f"</tr></thead><tbody>{''.join(rows)}</tbody></table></div>{''.join(cards)}</section>")
    reference = report["families"]["reference"]["metrics"]["raw_fusion"]
    page = f"""<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>M128 模型场景测试</title><style>
body{{background:#eef3f7;color:#183044;font:16px/1.65 'Microsoft YaHei',sans-serif;max-width:1450px;margin:32px auto;padding:0 24px}}
h1,h2{{line-height:1.3}}section,.intro{{background:white;border:1px solid #d5e0e8;border-radius:10px;padding:22px;margin:22px 0}}
.notice{{border-left:5px solid #d49b2d;padding:14px;background:#fff5dc}}.scroll{{overflow-x:auto}}table{{width:100%;border-collapse:collapse;font-size:14px}}
th,td{{padding:9px;border-bottom:1px solid #dce4eb;text-align:right;white-space:nowrap}}th:first-child,td:first-child{{text-align:left}}
img{{display:block;width:100%;height:auto;margin:14px 0}}details{{border-top:1px solid #dce4eb;padding:14px 0}}summary{{cursor:pointer}}code{{word-break:break-all}}
a{{color:#176a9b}}.muted{{color:#5f7384;font-size:14px}}</style><h1>冻结模型 · 新场景独立测试</h1>
<p>{report['sample_count']} 个预先定义样本，{report['independent_replicate_groups']} 个独立场景种子；组内只改变指定条件，配对样本不是独立重复。</p>
<div class="intro"><h2>首先看常规场景，再看压力边界</h2><p>常规组精确率 <b>{percent(reference['precision'])}</b>，召回率 <b>{percent(reference['recall'])}</b>，
匹配角误差 P95 <b>{number(reference['matched_angle_p95_deg'])}°</b>。所有压力条件不能混成真实应用准确率。</p>
<p class="notice">仅程序化视觉 + 模拟声学，不是实拍人物识别或 FPGA/MEMS 声学验收。已冻结模型和阈值，未重训或按本批测试调参。
图中的真值标记只用于报告，从未输入模型。失效通道、低信噪比和视觉退化可能造成高置信误报。</p>
<p>匹配门限 2°，一对一匹配。角误差仅统计匹配成功项，必须同时看漏检。严格近邻判据为每点误差≤min(2°,真实间隔/3)，预测间距≥真实间隔/2。
近邻通过不代表整场无误报；近邻率附 Wilson 95% 区间，小样本不能推出绝对分辨极限。</p>
<p>主表“模型”为强制执行冻结网络的原始结果；“门控拦截”为实际有效通道不足 112 或无有效声图的静态保护。
<a href="report.json">完整 JSON</a> 同时给出门控后结果。未模拟硬件同步、流时效或真正房间 RT60。</p>
<p class="muted">模型 SHA-256：<code>{report['model_sha256']}</code><br>阈值：目标 {report['thresholds']['confidence']}，发声 {report['thresholds']['active']}。
样例展示规则预先固定为每组首例、首个漏检、首个误报、首个完整正确，非挑选“最好看的”案例。</p></div>
<img src="report-assets/overview.png" alt="各测试族召回率概览">{''.join(sections)}
<section><h2>证据与复现</h2><p><a href="plan.json">执行前冻结计划</a> · <a href="report.json">所有分组指标与逐场景结果</a> ·
<a href="manifest.jsonl">输入与输出散列清单</a> · <a href="status.json">运行状态</a></p>
<p>完整 PCM、RGB、原始数值 SRP、模型输出和物理真值按场景保留。新场景种子、RGB/PCM 散列均与原训练/验证/测试集进行隔离检查。
不覆盖旧证据，不改模型文件和现场配置。显示伪彩不进入推理。</p></section></html>"""
    (output / "report.html").write_text(page, encoding="utf-8")
