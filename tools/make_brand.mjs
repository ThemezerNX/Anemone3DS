// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors
//
// Renders Themezer's logo from its design files into gfx/brand/ at the sizes the app draws it, so the GPU never scales
// it. Needs sharp, which the Themezer monorepo has:
//
//     node tools/make_brand.mjs [path/to/themezer-monorepo]

import path from 'node:path';
import os from 'node:os';
import fs from 'node:fs';
import { createRequire } from 'node:module';

const monorepo = process.argv[2] ?? path.join(os.homedir(), 'dev/themezer/themezer-monorepo');

// sharp from the monorepo: a normal install, or bun's package store
function loadSharp() {
    const require = createRequire(path.join(monorepo, 'package.json'));
    try {
        return require('sharp');
    } catch {
        const store = path.join(monorepo, 'node_modules', '.bun');
        const found = fs.readdirSync(store).filter((name) => name.startsWith('sharp@')).sort().pop();
        if (!found)
            throw new Error(`sharp isn't installed in ${monorepo}`);
        return require(path.join(store, found, 'node_modules', 'sharp'));
    }
}
const sharp = loadSharp();
const out = path.join(path.dirname(new URL(import.meta.url).pathname), '..', 'gfx', 'brand');
fs.mkdirSync(out, { recursive: true });

// [design file, output name, height in pixels]
const renders = [
    ['logo.svg', 'logo', 22],     // the top screen's bar
    ['icon.svg', 'icon_64', 64],  // the About screen and empty pages
];

for (const [file, name, height] of renders) {
    const source = path.join(monorepo, 'design', file);
    await sharp(source, { density: 1200, limitInputPixels: false })
        .resize({ height })
        .png()
        .toFile(path.join(out, `${name}.png`));
    console.log(name, height);
}

fs.writeFileSync(path.join(out, '..', 'brand.t3s'),
    ['--atlas -f rgba8 -z auto', ...renders.map(([, name]) => `brand/${name}.png`)].join('\n') + '\n');
