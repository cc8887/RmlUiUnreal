const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

(async () => {
  const directory = path.resolve(process.argv[2]);
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  try {
    const page = await browser.newPage();
    const rows = fs.readFileSync(path.join(directory, 'rects.tsv'), 'utf8').trim().split('\n').map(line => line.trim().split('\t'));
    let current = '', checked = 0;
    const differences = [];
    for (const [fixture, width, height, id, ...native] of rows) {
      if (fixture !== current) {
        await page.setViewportSize({ width: Number(width), height: Number(height) });
        await page.setContent('<html><head></head><body></body></html>');
        // RML fixtures are XML. Import through DOM APIs so self-closing divs retain their intended tree.
        await page.evaluate(xml => {
          const source = new DOMParser().parseFromString(xml, 'application/xml');
          if (source.querySelector('parsererror')) throw new Error('Invalid XML fixture');
          function copy(node) {
            if (node.nodeType === 3) return document.createTextNode(node.textContent);
            const element = document.createElement(node.tagName);
            for (const attr of node.attributes) element.setAttribute(attr.name, attr.value);
            for (const child of node.childNodes) if (child.nodeType === 1 || child.nodeType === 3) element.append(copy(child));
            return element;
          }
          document.documentElement.replaceChildren(...Array.from(source.documentElement.children, copy));
        }, fs.readFileSync(path.join(directory, `${fixture}.html`), 'utf8'));
        current = fixture;
      }
      const actual = await page.locator(`#${id}`).evaluate(el => { const r=el.getBoundingClientRect(); return [r.x,r.y,r.width,r.height]; });
      checked++;
      if (actual.some((v,i) => Math.abs(v-Number(native[i])) > 0.6)) differences.push({ fixture,id,native:native.map(Number),chromium:actual });
    }
    const result = { browser: await browser.version(), checked, differences, success: differences.length === 0 };
    fs.writeFileSync(path.join(directory,'chromium-comparison.json'), JSON.stringify(result,null,2));
    console.log(JSON.stringify(result,null,2));
    if (!result.success) process.exitCode=1;
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode=1; });
