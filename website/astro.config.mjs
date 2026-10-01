import { readdir, readFile } from 'node:fs/promises';
import { defineConfig } from 'astro/config';
import { satteri } from '@astrojs/markdown-satteri';
import sitemap from '@astrojs/sitemap';
import { links } from './src/links.ts';

// [Discord](links:discord) in Markdown links to links.discord, so the docs and the components share one list.
const siteLinks = {
  name: 'site-links',
  link(node, context) {
    const url = node.url.startsWith('links:') && links[node.url.slice('links:'.length)];
    if (url) context.setProperty(node, 'url', url);
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
  // The sitemap leaves out the redirects and the 404 page.
  integrations: [checkLinks, sitemap()],
  markdown: { processor: satteri({ mdastPlugins: [siteLinks] }) },
  vite: { server: { fs: { allow: ['..'] } } },
});
