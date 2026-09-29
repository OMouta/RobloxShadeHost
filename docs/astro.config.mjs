import { defineConfig } from 'astro/config';
import { satteri } from '@astrojs/markdown-satteri';

// The guides are the Markdown files at the root of the repository, which link to each other by file name.
const guideRoutes = {
  'INSTALLATION.md': '/docs/',
  'DLSS5-README.md': '/dlss5/',
};

const linkGuides = {
  name: 'link-guides',
  link(node, ctx) {
    if (guideRoutes[node.url]) ctx.setProperty(node, 'url', guideRoutes[node.url]);
  },
};

export default defineConfig({
  site: 'https://unishade.me',
  redirects: { '/install': '/docs' },
  trailingSlash: 'always',
  markdown: { processor: satteri({ mdastPlugins: [linkGuides] }) },
  // Screenshots and guides come from the repository around this folder.
  vite: { server: { fs: { allow: ['..'] } } },
});
