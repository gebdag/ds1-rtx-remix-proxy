"""Assemble lookdev\\hdr_out\\HDR10_REPORT.html from the hdr_shots.py runs (hdr_out\\<prefix>\\metrics.json).

    python hdr_report.py [--notes notes.html]

--notes: an HTML fragment (design, pitfalls, conclusions) placed before the measurements.
"""
import argparse
import html
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "hdr_out"


def fmt(v, digits=2):
    return "-" if v is None else f"{v:.{digits}f}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--notes", help="HTML fragment to include at the top")
    ap.add_argument("--out", default=str(OUT / "HDR10_REPORT.html"))
    args = ap.parse_args()

    runs = []
    for metrics in sorted(OUT.glob("*/metrics.json")):
        for m in json.loads(metrics.read_text(encoding="utf-8")):
            m["run"] = metrics.parent.name
            runs.append(m)

    rows = []
    for m in runs:
        tag = f"{m['capture']}_{'look' if m['look'] else 'stock'}"
        rel = f"{m['run']}/{tag}"
        rows.append(
            "<tr>"
            f"<td>{html.escape(m['run'])}</td><td>{html.escape(m['capture'])}</td><td>{'game look' if m['look'] else 'stock Remix'}</td>"
            f"<td>{fmt(m['unchanged_mean_8bit'])} / {fmt(m['unchanged_p99_8bit'], 1)}</td>"
            f"<td>{fmt(m.get('floor_mean_8bit'))} / {fmt(m.get('floor_p99_8bit'), 1)}</td>"
            f"<td>{m['sdr_clipped_share'] * 100:.1f}%</td><td>{m['hdr_above_white_share'] * 100:.1f}%</td>"
            f"<td>{fmt(m['clipped_headroom_stops_mean'])} / {fmt(m['clipped_headroom_stops_p95'])} / {fmt(m['headroom_stops_max'])}</td>"
            f"<td>{m['nits_p99']:.0f} / {m['nits_p999']:.0f} / {m['nits_max']:.0f}</td>"
            f"<td>{fmt(m['clipped_saturation_sdr'])} &rarr; {fmt(m['clipped_saturation_hdr'])}</td>"
            f"<td><a href='{rel}_sheet.png'>sheet</a> <a href='{rel}_sdr.png'>sdr</a> <a href='{rel}_hdr_view.png'>hdr</a> "
            f"<a href='{rel}_headroom.png'>headroom</a> <a href='{rel}_hdr_window.png'>window</a></td>"
            "</tr>")

    sheets = "\n".join(
        f"<figure><img src='{m['run']}/{m['capture']}_{'look' if m['look'] else 'stock'}_sheet.png' loading='lazy'>"
        f"<figcaption>{html.escape(m['run'])} &middot; {html.escape(m['capture'])} &middot; {'game look' if m['look'] else 'stock Remix'}: "
        f"SDR frame | HDR frame with the display peak as white | headroom map (grey = within SDR, blue&rarr;red = 0&rarr;2 stops above white)</figcaption></figure>"
        for m in runs)

    notes = Path(args.notes).read_text(encoding="utf-8") if args.notes else ""
    doc = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>DS1 RTX Remix: HDR10 output</title>
<style>
body{{font-family:Segoe UI,Arial,sans-serif;margin:24px auto;max-width:1500px;color:#222;line-height:1.4}}
table{{border-collapse:collapse;font-size:13px}} th,td{{border:1px solid #ccc;padding:4px 7px;text-align:left;vertical-align:top}} th{{background:#eee}}
figure{{margin:18px 0}} figure img{{max-width:100%;border:1px solid #ccc}} figcaption{{font-size:13px;color:#444}}
code{{background:#f3f3f3;padding:1px 4px}} h2{{margin-top:36px}}
</style></head><body>
<h1>Dark Souls RTX Remix: HDR10 output (experimental branch <code>hdr10</code>)</h1>
{notes}
<h2>Measurements</h2>
<p>Per capture, the host rendered the frame with HDR off and on and the runtime dumped its final frame both times
(16-bit float, gamma-encoded, 1.0 = SDR white). <b>unchanged</b>: mean / 99th percentile of |HDR &minus; SDR| in
8-bit steps over the pixels SDR did not clip (two different renders: the path-tracing noise floor is 0.2-0.5 of a step
for the game look, more for stock Remix whose auto exposure drifts). <b>SDR clipped</b>: share of pixels with a channel
at the top 8-bit step. <b>HDR above white</b>: share of pixels the HDR frame holds above 1.0. <b>headroom</b>: stops above
SDR white in the clipped pixels (mean / p95) and the frame's maximum. <b>nits</b>: luminance p99 / p99.9 / max after the
present mapping (paper white {runs[0]['paper_white'] if runs else '-'} nits, peak {runs[0]['peak'] if runs else '-'} nits,
roll-off from {runs[0]['shoulder'] if runs else '-'} of peak). <b>chroma</b>: saturation (max&minus;min)/max in the clipped
pixels, SDR &rarr; HDR.</p>
<table><tr><th>run</th><th>capture</th><th>path</th><th>unchanged (8-bit)</th><th>floor SDR&rarr;SDR (8-bit)</th><th>SDR clipped</th><th>HDR &gt; white</th>
<th>headroom stops mean / p95 / max</th><th>nits p99 / p99.9 / max</th><th>chroma SDR&rarr;HDR</th><th>images</th></tr>
{''.join(rows)}
</table>
<h2>Sheets</h2>
{sheets}
</body></html>"""
    Path(args.out).write_text(doc, encoding="utf-8")
    print(args.out, len(runs), "runs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
