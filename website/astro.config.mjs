import { defineConfig } from 'astro/config';
import { satteri } from '@astrojs/markdown-satteri';

export default defineConfig({
  site: 'https://unishade.me',
  redirects: {
    '/install': '/docs/',
    '/dlss5': '/docs/add-ons/#dlss5',
  },
  trailingSlash: 'always',
  markdown: { processor: satteri() },
  vite: { server: { fs: { allow: ['..'] } } },
});
