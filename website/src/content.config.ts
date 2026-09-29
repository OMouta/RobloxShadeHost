import { defineCollection } from 'astro:content';
import { glob } from 'astro/loaders';

// The same files GitHub shows, so the guides are written once.
const guides = defineCollection({
  loader: glob({ pattern: ['INSTALLATION.md', 'DLSS5-README.md'], base: '..' }),
});

export const collections = { guides };
