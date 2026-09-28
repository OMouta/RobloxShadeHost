export const links = {
  download: 'https://github.com/OMouta/RobloxShadeHost/releases/latest',
  discord: 'https://discord.gg/wVbVUdENas',
  github: 'https://github.com/OMouta/RobloxShadeHost',
  license: 'https://github.com/OMouta/RobloxShadeHost/blob/main/LICENSE',
  reshade: 'https://reshade.me',
};

// A page of this site, under the base path GitHub Pages serves it from.
export const page = (path = '') => `${import.meta.env.BASE_URL.replace(/\/?$/, '/')}${path}`;
