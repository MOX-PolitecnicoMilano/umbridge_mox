#!/bin/bash
# Build PBS_Deployment_Guide.pdf from PBS_Deployment_Guide.md.
# Mermaid blocks are rendered to PNG with mmdc (@mermaid-js/mermaid-cli) and
# substituted in a temporary copy; the Markdown file is never modified.
# Requires: pandoc, xelatex, google-chrome, mmdc (npm install -g @mermaid-js/mermaid-cli;
# with a user prefix, make sure ~/.local/bin is in PATH), python3.
# Usage: ./make-guide-pdf.sh [file.md]   (default: PBS_Deployment_Guide.md)

set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
md=${1:-$here/PBS_Deployment_Guide.md}
pdf=${md%.md}.pdf
export PATH="$HOME/.local/bin:$PATH"

for tool in pandoc xelatex mmdc python3; do
	command -v "$tool" > /dev/null || { echo "Missing required tool: $tool" >&2; exit 1; }
done
chrome=$(command -v google-chrome || command -v chromium || command -v chromium-browser) \
	|| { echo "Chrome/Chromium not found" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
printf '{"executablePath":"%s","args":["--no-sandbox"]}\n' "$chrome" > "$work/puppeteer.json"

# Split out the mermaid blocks and build the substituted Markdown copy.
python3 - "$md" "$work" <<'EOF'
import re, sys
md, work = sys.argv[1:3]
text = open(md, encoding="utf-8").read()
count = 0
def sub(m):
    global count
    count += 1
    body = m.group(1)
    # A wide left-to-right flowchart gets unreadably small on a page.
    body = re.sub(r"^flowchart LR", "flowchart TB", body, flags=re.M)
    open(f"{work}/d{count}.mmd", "w", encoding="utf-8").write(body)
    width = "50%" if body.lstrip().startswith("flowchart") else "95%"
    return f"![Diagram {count}]({work}/d{count}.png){{width={width}}}"
out = re.sub(r"```mermaid\n(.*?)```", sub, text, flags=re.S)
open(f"{work}/guide.md", "w", encoding="utf-8").write(out)
print(count)
EOF

for f in "$work"/d*.mmd; do
	mmdc -q -p "$work/puppeteer.json" -s 3 -b white -i "$f" -o "${f%.mmd}.png"
done

pandoc "$work/guide.md" -o "$pdf" --pdf-engine=xelatex --toc --toc-depth=2 \
	-V geometry:margin=2.2cm -V fontsize=10pt -V colorlinks=true \
	-V monofont="DejaVu Sans Mono" -V mainfont="DejaVu Serif" \
	--highlight-style=tango --resource-path="$(dirname -- "$md")"
echo "Created $pdf"
