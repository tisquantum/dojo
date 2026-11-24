# Writes the README's logo, docs/logo-dark.svg and docs/logo-light.svg: "DOJO" in geometric block letters, the last O cut by a red
# sword stroke, under a small letter-spaced line. Letters are paths, so they look the same everywhere.
import os

DOCS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs")

LETTERS = {
    "D": "M0,0 H100 L150,50 V90 L100,140 H0 Z M34,34 V106 H86 L116,76 V64 L86,34 Z",
    "O": "M40,0 H110 L150,40 V100 L110,140 H40 L0,100 V40 Z M52,34 L34,52 V88 L52,106 H98 L116,88 V52 L98,34 Z",
    "J": "M50,0 H150 V100 L110,140 H40 L0,100 V78 H34 V88 L52,106 H98 L116,88 V34 H50 Z",
}
W, GAP, X0, Y0 = 150, 34, 0, 0
word = "DOJO"
total = len(word) * W + (len(word) - 1) * GAP
width, height = 820, 250
ox = (width - total) / 2
oy = 82

def svg(fg, sub, accent):
    parts = []
    for i, ch in enumerate(word):
        x = ox + i * (W + GAP)
        parts.append('<path transform="translate(%.1f %d)" fill-rule="evenodd" d="%s"/>' % (x, oy, LETTERS[ch]))
    # The cut: a gap through the last O, and the blade's stroke along it.
    lx = ox + 3 * (W + GAP)
    x1, y1, x2, y2 = lx - 22, oy + 160, lx + W + 22, oy - 20
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}">
  <defs>
    <mask id="cut" maskUnits="userSpaceOnUse" x="0" y="0" width="{width}" height="{height}">
      <rect width="{width}" height="{height}" fill="#fff"/>
      <line x1="{x1:.1f}" y1="{y1}" x2="{x2:.1f}" y2="{y2}" stroke="#000" stroke-width="22"/>
    </mask>
  </defs>
  <text x="{width / 2}" y="52" text-anchor="middle" fill="{sub}" font-family="Segoe UI, Helvetica, Arial, sans-serif"
        font-size="22" font-weight="700" letter-spacing="9">BUSHIDO BLADE · PC</text>
  <g fill="{fg}" mask="url(#cut)">
    {chr(10).join("    " + p for p in parts).strip()}
  </g>
  <line x1="{x1:.1f}" y1="{y1}" x2="{x2:.1f}" y2="{y2}" stroke="{accent}" stroke-width="6" stroke-linecap="round"/>
</svg>
'''

open(os.path.join(DOCS, "logo-dark.svg"), "w", encoding="utf-8").write(svg("#FFFFFF", "#9AA4B2", "#E5383B"))
open(os.path.join(DOCS, "logo-light.svg"), "w", encoding="utf-8").write(svg("#111827", "#6B7280", "#D62828"))
