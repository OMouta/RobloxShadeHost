import { readdir, readFile } from 'node:fs/promises';
import { defineConfig, fontProviders } from 'astro/config';
import { satteri } from '@astrojs/markdown-satteri';
import sitemap from '@astrojs/sitemap';
import { links } from './src/links.ts';

// [Discord](links:discord) in Markdown links to links.discord, so the docs and the components share one list.
const siteLinks = {
  name: 'site-links',
  link(node, context) {
    const name = node.url.startsWith('links:') && node.url.slice('links:'.length);
    if (name && Object.hasOwn(links, name)) context.setProperty(node, 'url', links[name]);
  },
};

// Screenshots in the docs are at most as wide as the docs column, so phones and 2x screens get the size they show.
const imageSizes = {
  name: 'image-sizes',
  element: {
    filter: ['img'],
    visit(node, context) {
      context.setProperty(node, 'sizes', '(min-width: 860px) 720px, 100vw');
    },
  },
};

// A name that isn't in links.ts stays as it is, and fails the build here.
const checkLinks = {
  name: 'check-links',
  hooks: {
    'astro:build:done': async ({ dir }) => {
      for (const file of await readdir(dir, { recursive: true })) {
        if (!file.endsWith('.html')) continue;
        const unknown = /href="(links:[^"]*)"/.exec(await readFile(new URL(file, dir), 'utf8'));
        if (unknown) throw new Error(`${file} links to ${unknown[1]}, which isn't in src/links.ts.`);
      }
    },
  },
};

export default defineConfig({
  site: 'https://unishade.me',
  redirects: {
    '/install': '/docs/',
    '/dlss5': '/docs/add-ons/#dlss5',
  },
  trailingSlash: 'always',
  // A Content-Security-Policy <meta> on every page, with hashes of the scripts and styles Astro inlines. GitHub Pages
  // can't send headers, so frame-ancestors and reporting aren't possible.
  security: {
    csp: { directives: ["default-src 'self'", "object-src 'none'", "base-uri 'none'", "form-action 'none'"] },
  },
  // The sitemap leaves out the redirects and the 404 page.
  integrations: [checkLinks, sitemap()],
  // Archivo from @fontsource-variable/archivo, its Latin letters only. <Font preload /> in Base.astro preloads it, and the
  // fallback is Arial resized to Archivo's measurements, so the text barely moves when Archivo arrives.
  fonts: [
    {
      provider: fontProviders.local(),
      name: 'Archivo',
      cssVariable: '--font-archivo',
      fallbacks: ['system-ui', 'sans-serif'],
      options: {
        variants: [
          {
            src: ['@fontsource-variable/archivo/files/archivo-latin-wdth-normal.woff2'],
            weight: '100 900',
            style: 'normal',
            stretch: '62% 125%',
          },
        ],
      },
    },
  ],
  // Images get a srcset of sizes up to the original's.
  image: { layout: 'constrained' },
  markdown: {
    processor: satteri({ mdastPlugins: [siteLinks], hastPlugins: [imageSizes] }),
    // Shiki colours code with style attributes, which the CSP blocks. The docs show code without colours.
    syntaxHighlight: false,
  },
  vite: { server: { fs: { allow: ['..'] } } },
});
