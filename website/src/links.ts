export const links = {
  download: 'https://unishade.me/download',
  release: 'https://github.com/OMouta/Unishade/releases/latest',
  discord: 'https://discord.gg/wVbVUdENas',
  github: 'https://github.com/OMouta/Unishade',
  license: 'https://github.com/OMouta/Unishade/blob/main/LICENSE',
  reshade: 'https://reshade.me',
};

// A page of the canonical site.
export const page = (path = '') => `${import.meta.env.BASE_URL.replace(/\/?$/, '/')}${path}`;
