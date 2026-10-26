import fs from 'node:fs';
import path from 'node:path';
import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// Docs for the hugr-lab DuckDB MSSQL extension. Mirrors the org site's
// Docusaurus setup (hugr-lab.github.io) so the two share look and feel;
// published by this repo's Pages workflow to /mssql-extension alongside the
// download-metrics chart (served under /metrics/, SVG kept at its stable
// root path because the README embeds it).

// The same docs are built twice: for GitHub Pages (default) and for
// hugr-lab.com on IONOS (DOCS_TARGET=ionos, uploaded by pages.yml). The IONOS
// copy is served under a Content-Security-Policy that admits nothing from
// another host, and the site's Datenschutz says so: no image, font, script or
// fetch may leave hugr-lab.com. A plain <a href> to elsewhere is fine. So that
// copy drops every markdown image with an absolute URL (markdown.preprocessor
// below: the shields.io badges, in released versions too), the download-metrics
// chart is the build's own copy (static/download-metrics.svg) in both, and the
// /metrics/ page -- DuckDB-Wasm and Plot from a CDN -- is added to the GitHub
// Pages copy only; both copies link to that one.
// scripts/ci/check_docs_no_third_party.py checks the result.
const ionos = process.env.DOCS_TARGET === 'ionos';
const site = ionos
  ? {url: 'https://hugr-lab.com', baseUrl: '/docs/mssql-extension/', home: 'https://hugr-lab.com/', impressum: 'https://hugr-lab.com/impressum/', datenschutz: 'https://hugr-lab.com/datenschutz/'}
  : {url: 'https://hugr-lab.github.io', baseUrl: '/mssql-extension/', home: 'https://hugr-lab.github.io/', impressum: 'https://hugr-lab.github.io/impressum/', datenschutz: 'https://hugr-lab.github.io/datenschutz/'};

// While the hugr-lab.com copy exists (DOCS_IONOS on), pages.yml sets DOCS_CANONICAL for
// the GitHub Pages build: its pages then name the hugr-lab.com copy as canonical
// (src/theme/SiteMetadata) and it has no sitemap of its own. Search engines index
// hugr-lab.com; the github.io links keep working (owner's decision, 2026-10-19).
const canonicalBase = ionos ? '' : process.env.DOCS_CANONICAL ?? '';

// Only the latest release -- the version at the docs root, versions.json's first
// entry -- is indexed. `next` (current) and the older releases are near-copies of
// it, each naming itself canonical, and Search Console reported them as
// duplicates whose canonical Google disagreed with (owner's decision,
// 2026-10-26). They get `noindex, nofollow` and drop out of the sitemap
// (plugin-sitemap skips no-index routes); the version dropdown keeps them.
const versionsFile = path.join(__dirname, 'versions.json');
const released: string[] = fs.existsSync(versionsFile) ? JSON.parse(fs.readFileSync(versionsFile, 'utf8')) : [];
const noIndexVersions = Object.fromEntries(
  released.length ? ['current', ...released.slice(1)].map((v) => [v, {noIndex: true}]) : [],
);

const config: Config = {
  title: 'DuckDB MSSQL Extension',
  tagline: 'SQL Server and Azure SQL from DuckDB — native TDS, no ODBC, built for speed.',
  favicon: 'img/favicon.ico',

  url: site.url,
  baseUrl: site.baseUrl,
  trailingSlash: true,
  // The banner is an inline script that guesses at a misconfigured baseUrl;
  // both baseUrls here are deliberate.
  baseUrlIssueBanner: false,
  customFields: {docsTarget: ionos ? 'ionos' : 'github', canonicalBase},

  organizationName: 'hugr-lab',
  projectName: 'mssql-extension',

  onBrokenLinks: 'throw',
  // 'throw', not 'warn': docs-build.yml is the PR gate for website/, and a
  // gate that exits 0 on a broken cross-page anchor does not gate anything.
  // A dangling anchor is exactly the breakage that reached main from #286 --
  // the build reported it and passed. onBrokenMarkdownLinks lives under
  // markdown.hooks now; the top-level key is deprecated in Docusaurus 3.10
  // and removed in v4.
  onBrokenAnchors: 'throw',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.ts',
          // Docs ARE the site: /mssql-extension/<page>/
          routeBasePath: '/',
          // VERSIONING CONTRACT: at each release, run
          //   npm run docusaurus docs:version <X.Y.Z>
          // in website/ and commit the snapshot. Docusaurus then serves the
          // latest RELEASED version at the root (the default) and the live
          // docs/ tree as "Next" under /next/ with an "unreleased" banner —
          // so in-progress docs for the coming release are reachable but
          // never the landing default. Until the first snapshot exists,
          // current docs serve at the root with no dropdown entry to switch.
          editUrl: 'https://github.com/hugr-lab/mssql-extension/tree/main/website/',
          showLastUpdateTime: true,
          versions: noIndexVersions,
        },
        blog: false,
        // The canonical copy (hugr-lab.com) carries the sitemap; a copy that names
        // another as canonical has none.
        sitemap: canonicalBase ? false : undefined,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themes: ['@docusaurus/theme-mermaid'],

  markdown: {
    mermaid: true,
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
    // The hugr-lab.com copy shows no image from another server: badges in released versions included.
    preprocessor: ({fileContent}) =>
      ionos ? fileContent.replace(/\[!\[[^\]]*\]\(https?:\/\/[^)]*\)\]\([^)]*\)|!\[[^\]]*\]\(https?:\/\/[^)]*\)/g, '') : fileContent,
  },

  themeConfig: {
    metadata: [
      {name: 'keywords', content: 'DuckDB, SQL Server, Azure SQL, MSSQL, TDS, extension, BCP, bulk load, Fabric, Kerberos, Azure AD'},
      {name: 'description', content: 'DuckDB extension for Microsoft SQL Server and Azure SQL: attach databases, stream reads, bulk-load writes over a native TDS implementation.'},
    ],
    navbar: {
      title: 'MSSQL Extension',
      logo: {
        alt: 'Hugr Lab',
        src: 'img/logo-circle.svg',
        href: '/',
      },
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'docsSidebar',
          position: 'left',
          label: 'Docs',
        },
        {
          type: 'docsVersionDropdown',
          position: 'right',
        },
        {
          href: 'https://hugr-lab.github.io/mssql-extension/metrics/',
          label: 'Downloads',
          position: 'left',
        },
        {
          href: site.home,
          label: 'Hugr Lab',
          position: 'right',
        },
        {
          href: 'https://github.com/hugr-lab/mssql-extension',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    colorMode: {
      defaultMode: 'light',
      disableSwitch: true,
      respectPrefersColorScheme: false,
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Docs',
          items: [
            {label: 'Getting Started', to: '/getting-started/'},
            {label: 'Settings Reference', to: '/reference/settings/'},
            {label: 'Performance', to: '/performance/'},
          ],
        },
        {
          title: 'Community',
          items: [
            {label: 'GitHub', href: 'https://github.com/hugr-lab/mssql-extension'},
            {label: 'Issues', href: 'https://github.com/hugr-lab/mssql-extension/issues'},
            {label: 'DuckDB Community Extensions', href: 'https://duckdb.org/community_extensions/extensions/mssql.html'},
          ],
        },
        {
          title: 'Hugr Lab',
          items: [
            {label: 'Main site', href: site.home},
            {label: 'Articles', to: '/articles/'},
          ],
        },
        {
          title: 'Legal',
          items: [
            {label: 'Impressum', href: site.impressum},
            {label: 'Datenschutz', href: site.datenschutz},
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} Hugr Lab.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['sql', 'bash', 'powershell'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
