// The newest release, read from GitHub once per build. Publishing a release rebuilds the site, so the download
// buttons link straight to the newest Setup.

interface Asset {
  name: string;
  size: number;
  browser_download_url: string;
}

interface GitHubRelease {
  tag_name: string;
  draft: boolean;
  prerelease: boolean;
  published_at: string;
  html_url: string;
  assets: Asset[];
}

// The repository being built in GitHub Actions. GitHub forwards the old name once the repository is renamed.
const repository = process.env.GITHUB_REPOSITORY ?? 'OMouta/RobloxShadeHost';

function version(tag: string) {
  const match = /^v(\d+)\.(\d+)\.(\d+)$/.exec(tag);
  return match ? match.slice(1).map(Number) : undefined;
}

function newer(a: number[], b: number[]) {
  const index = a.findIndex((part, i) => part !== b[i]);
  return index >= 0 && a[index] > b[index];
}

async function newest() {
  const headers: Record<string, string> = { Accept: 'application/vnd.github+json' };
  // Without a token GitHub allows 60 requests an hour, which local builds rarely reach.
  if (process.env.GITHUB_TOKEN) headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
  const response = await fetch(`https://api.github.com/repos/${repository}/releases?per_page=30`, { headers });
  if (!response.ok) throw new Error(`Could not list the releases of ${repository}: ${response.status} ${response.statusText}`);

  // The depth and DLSS5 files are releases too, so only vX.Y.Z tags count, like the host's update check.
  let best: { release: GitHubRelease; setup: Asset; version: number[] } | undefined;
  for (const release of (await response.json()) as GitHubRelease[]) {
    const parts = version(release.tag_name);
    const setup = release.assets.find((asset) => /-Setup-\d+\.\d+\.\d+\.exe$/.test(asset.name));
    if (parts && setup && !release.draft && !release.prerelease && (!best || newer(parts, best.version)))
      best = { release, setup, version: parts };
  }
  if (!best) throw new Error(`${repository} has no release with a Setup EXE.`);

  const { release, setup } = best;
  // Releases before the rename call the host RobloxShadeHost.exe.
  const host = release.assets.find((asset) => /^(Unishade|RobloxShadeHost)\.exe$/.test(asset.name));
  return {
    version: release.tag_name.slice(1),
    date: new Date(release.published_at),
    notes: release.html_url,
    setup: { url: setup.browser_download_url, size: setup.size },
    host: host && { url: host.browser_download_url, size: host.size },
  };
}

export const release = await newest();

export const megabytes = (bytes: number) => `${(bytes / 1024 / 1024).toFixed(1)} MB`;
